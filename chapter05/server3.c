/*
 * server3: poll() による “擬似マルチクライアント” TCP サーバ
 *
 * 目的：
 * - server2 の select() 版と同じ思想を、poll() で書き直した例
 * - fork/thread を使わず 1プロセス/1スレッドで複数接続を扱う
 * - poll() は select と比べて
 *   - fd_set のビット制限（FD_SETSIZE）に縛られにくい
 *   - “監視対象を配列で表す” ため、最大FDの計算（width）が不要
 *   というメリットがあり、学習段階で比較しやすい
 *
 * 全体アルゴリズム：
 * 1) server_socket(port) で listen ソケットを作る
 * 2) accept_loop(listen_fd) でイベントループを回す
 *    - pollfd 配列 targets[] を毎回作る
 *      - targets[0] = listen_fd（新規接続監視）
 *      - targets[1..] = accept 済みの接続FD（既存クライアント監視）
 *    - poll(targets, count, timeout_ms) を呼び、読み込み可能イベントを待つ
 *    - targets[0] が POLLIN → accept して child[] に登録
 *    - targets[i] が POLLIN/POLLERR → send_recv() を1回実行
 *      - エラー/EOF なら close して child[] の該当FDを -1 に戻す
 *
 * 重要：
 * - “同時並列” ではなく “イベント駆動で順番に捌く” モデル
 * - poll/ select 方式では、各イベント処理（send_recv）が短いことが前提
 *   （重い処理を入れると全体が止まる）
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <poll.h>                       /* poll(), struct pollfd, POLLIN, POLLERR */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/* サーバソケットの準備（listen ソケットを作る）
 *
 * portnm: 文字列のポート番号（例: "55555"）
 *
 * アルゴリズム：
 * - getaddrinfo(NULL, port, AI_PASSIVE) で待受けアドレスを取得
 * - socket → setsockopt(SO_REUSEADDR) → bind → listen
 *
 * 戻り値：
 * - 成功：listen ソケット FD
 * - 失敗：-1
 */
int
server_socket(const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    /* アドレス情報のヒントをゼロクリア */
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */
    hints.ai_flags = AI_PASSIVE;      /* 待受け用 */

    /* アドレス情報の決定 */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* デバッグ表示（バインド対象の数値ポートを表示） */
    if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen,
                               nbuf, sizeof(nbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
        freeaddrinfo(res0);
        return (-1);
    }
    (void) fprintf(stderr, "port=%s\n", sbuf);

    /* ソケット生成 */
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }

    /* 再起動時の bind 失敗を減らす（TIME_WAIT の影響を受けにくくする） */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* bind：ポートに割り当て */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* listen：接続受付状態へ */
    if (listen(soc, SOMAXCONN) == -1) {
        perror("listen");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    freeaddrinfo(res0);
    return (soc);
}

/* 最大同時 “接続管理” 数（accept 済み接続 FD の最大保持数） */
#define MAX_CHILD (20)

/* poll() を使った accept ループ（イベントループ）
 *
 * soc: listen ソケット FD
 *
 * この実装のデータ構造：
 * - child[]：accept 済みの “接続FD” を保持する配列（-1 が空き）
 * - targets[]：poll() に渡す pollfd 配列
 *   - targets[0] は listen FD
 *   - targets[1..count-1] は child[] の有効FDを “詰めて” 格納する
 *
 * 重要な違い（select vs poll）：
 * - select は fd_set を構築し “最大FD+1(width)” が必要
 * - poll は pollfd 配列を渡すだけなので width 計算が不要
 * - poll の結果は revents に出る（どのイベントが起きたか）
 */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    int child[MAX_CHILD];
    struct sockaddr_storage from;
    int acc, child_no, i, j, count, pos, ret;
    socklen_t len;

    /* poll() に渡す監視対象の配列
     * +1 は listen FD（targets[0]）用
     */
    struct pollfd targets[MAX_CHILD + 1];

    /* child 配列の初期化（-1 が空きスロット） */
    for (i = 0; i < MAX_CHILD; i++) {
        child[i] = -1;
    }

    /* child_no：配列上 “使う範囲” の上限（末尾管理）
     * - 空き(-1)が途中にあっても 0..child_no-1 を走査する
     */
    child_no = 0;

    for (;;) {
        /* 1) poll() 用データ（targets[]）を毎回構築する
         *
         * count は targets に詰めた要素数（poll に渡す nfds）で、
         * targets[0] は listen FD なので count は最低 1
         */
        count = 0;

        /* targets[0] = listen ソケット（新規接続受付イベント）
         * events= POLLIN：読み込み可能（＝接続待ちキューに何かある）
         */
        targets[count].fd = soc;
        targets[count].events = POLLIN;
        targets[count].revents = 0;   /* 念のためクリア（poll が上書きするが読み手に明確） */
        count++;

        /* 既存接続（child[]）を targets[1..] に詰める */
        for (i = 0; i < child_no; i++) {
            if (child[i] != -1) {
                targets[count].fd = child[i];
                targets[count].events = POLLIN;  /* 受信可能を監視 */
                targets[count].revents = 0;
                count++;
            }
        }

        (void) fprintf(stderr, "<<child count:%d>>\n", count - 1);

        /* 2) poll で “イベント待ち”
         * 第3引数はタイムアウト（ms）
         * - ここでは 10秒 = 10*1000ms
         *
         * poll の返り値：
         * - -1 : エラー
         * -  0 : タイムアウト（何も起きてない）
         * - >0 : 何かの fd にイベントが来た（revents を見る）
         */
        switch (poll(targets, count, 10 * 1000)) {
        case -1:
            perror("poll");
            break;

        case 0:
            /* タイムアウト：ここでは何もしない（ループ継続） */
            break;

        default:
            /* 3) ready な FD を処理する */

            /* (a) targets[0]（listen FD）に POLLIN → accept */
            if (targets[0].revents & POLLIN) {
                len = (socklen_t) sizeof(from);

                /* accept：接続専用 FD（acc）を得る */
                if ((acc = accept(soc, (struct sockaddr *)&from, &len)) == -1) {
                    if (errno != EINTR) {
                        perror("accept");
                    }
                } else {
                    /* 接続元の数値アドレス/ポート表示 */
                    (void) getnameinfo((struct sockaddr *) &from, len,
                                       hbuf, sizeof(hbuf),
                                       sbuf, sizeof(sbuf),
                                       NI_NUMERICHOST | NI_NUMERICSERV);
                    (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

                    /* child[] の空きを探す（-1 が空き） */
                    pos = -1;
                    for (i = 0; i < child_no; i++) {
                        if (child[i] == -1) {
                            pos = i;
                            break;
                        }
                    }

                    if (pos == -1) {
                        /* 空きが無い：child_no を伸ばして末尾に追加できるか */
                        if (child_no + 1 >= MAX_CHILD) {
                            /* これ以上保持できない：受けた接続は捨てる */
                            (void) fprintf(stderr, "child is full : cannot accept\n");
                            (void) close(acc);
                        } else {
                            child_no++;
                            pos = child_no - 1;
                        }
                    }

                    if (pos != -1) {
                        /* 接続FDを登録（次回 poll の監視対象に入る） */
                        child[pos] = acc;
                    }
                }
            }

            /* (b) targets[1..]（既存接続）で POLLIN/POLLERR を処理
             *
             * - POLLIN : 読み込み可能（recv できる）
             * - POLLERR: エラー（ソケット異常）
             *
             * 注意：
             * - pollfd 配列 targets は “詰めた配列” なので、
             *   i は child のインデックスそのものではない
             * - ここではログ表示用 child_no を i-1 として渡している
             *   （教材として「targets上の番号」を child番号にしている）
             */
            for (i = 1; i < count; i++) {
                if (targets[i].revents & (POLLIN | POLLERR)) {
                    /* 送受信（1回分） */
                    if ((ret = send_recv(targets[i].fd, i - 1)) == -1) {
                        /* エラー/切断：クローズして child[] からも削除 */
                        (void) close(targets[i].fd);

                        /* child[] 側の該当FDを探して -1（空き）に戻す
                         * - targets は “詰め配列” なので、child[] の位置が分からないため探索が必要
                         */
                        for (j = 0; j < child_no; j++) {
                            if (child[j] == targets[i].fd) {
                                child[j] = -1;
                                break;
                            }
                        }
                    }
                }
            }
            break;
        }
    }
}

/* サイズ指定文字列連結（strlcat 相当の安全版）
 * - 応答 ":OK\r\n" を付ける際のバッファオーバーフローを防ぐ
 */
size_t
mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--)
        ;
    dlen = pd - dst;

    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }

    pde = dst + size - 1;

    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }

    while (*ps++)
        ;
    return (dlen + (ps - src - 1));
}

/* 送受信（1回分）
 *
 * acc      : 接続FD
 * child_no : ログ表示用番号（ここでは “targets上の順序”）
 *
 * アルゴリズム：
 * - recv → EOF/エラーなら -1
 * - 受信内容を表示し、":OK\r\n" を付けて send で返す
 *
 * 注意：
 * - buf[len] = '\0' は len==sizeof(buf) のとき境界外になる可能性がある
 *   → recv のサイズを sizeof(buf)-1 にするのが安全
 * - send は部分送信があり得る（教材として単純化）
 */
int
send_recv(int acc, int child_no)
{
    char buf[512], *ptr;
    ssize_t len;

    /* 受信 */
    if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
        perror("recv");
        return (-1);
    }
    if (len == 0) {
        (void) fprintf(stderr, "[child%d]recv:EOF\n", child_no);
        return (-1);
    }

    /* 文字列化（終端付与）
     * NOTE: len==512 のとき buf[512] で境界外になり得る
     */
    buf[len] = '\0';

    /* 改行を切ってログを1行に揃える */
    if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
        *ptr = '\0';
    }

    (void) fprintf(stderr, "[child%d]%s\n", child_no, buf);

    /* 応答文字列作成 */
    (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
    len = strlen(buf);

    /* 応答送信 */
    if ((len = send(acc, buf, len, 0)) == -1) {
        perror("send");
        return (-1);
    }

    return (0);
}

int
main(int argc, char *argv[])
{
    int soc;

    /* 引数チェック */
    if (argc <= 1) {
        (void) fprintf(stderr, "server3 port\n");
        return (EX_USAGE);
    }

    /* listen ソケット準備 */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    (void) fprintf(stderr, "ready for accept\n");

    /* poll() ベースのイベントループ */
    accept_loop(soc);

    /* 実際には accept_loop は戻らない想定だが形式上 close */
    (void) close(soc);
    return (EX_OK);
}

/*
 * poll() 多重化の学習ポイントまとめ
 *
 * 1) select と同様に「監視対象集合を毎回作る」思想は同じ
 *    - select: fd_set を作る
 *    - poll  : pollfd 配列を作る
 *
 * 2) poll は width（最大FD+1）が不要
 *    - “配列で監視対象を渡す” ため
 *
 * 3) イベント判定は FD_ISSET ではなく revents を見る
 *    - POLLIN / POLLERR / POLLHUP などのビットで何が起きたか分かる
 *
 * 4) 大量FDでのスケール
 *    - poll は select より扱いやすいが、依然 O(N) で配列全走査が必要
 *    - Linux なら epoll、BSD/macOS なら kqueue がさらにスケールする
 */
