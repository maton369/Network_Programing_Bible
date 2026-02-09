/*
 * server2: select() による “擬似マルチクライアント” TCP サーバ
 *
 * 目的：
 * - fork/thread を使わず、1プロセス/1スレッドで複数クライアントを扱う
 * - select() による I/O 多重化（multiplexing）を学ぶ
 *
 * 全体アルゴリズム（サーバ側）：
 * 1) server_socket(port) で listen ソケットを準備する
 *    - getaddrinfo → socket → setsockopt(SO_REUSEADDR) → bind → listen
 *
 * 2) accept_loop(listen_fd) でイベントループを回す
 *    - 監視対象 FD の集合（fd_set）を毎回作り直す
 *      - listen_fd（新規接続受付用）
 *      - 既に accept 済みの接続 FD（child[] 配列で管理）
 *    - select() で “読み込み可能” になるまで待つ（タイムアウト付き）
 *    - ready になった FD に対して処理する
 *      - listen_fd が ready → accept() して child[] の空きに接続 FD を登録
 *      - child[i] が ready → recv/send（send_recv）を1回実行
 *        - エラー/切断なら close して child[i] を空き(-1)に戻す
 *
 * 重要なポイント：
 * - これは “同時処理” ではなく “イベント駆動で順番に捌く” 方式
 * - select は「いま read できる FD」を教えてくれるので、ブロックせずに多重化できる
 * - child[] は “接続済みソケットの集合” を表しており、ここでは最大 MAX_CHILD 件まで扱う
 *
 * 注意：
 * - recv/send は TCP のストリーム性（部分受信/部分送信）を単純化している
 * - buf[len]='\0' は len==sizeof(buf) の場合に境界外アクセスになり得る（後述）
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
 * 1) hints を設定して getaddrinfo(NULL, port, ...) を呼ぶ
 *    - host=NULL + AI_PASSIVE なので “待受け用アドレス（0.0.0.0相当）” を得る
 * 2) socket() でソケット生成
 * 3) setsockopt(SO_REUSEADDR) で再起動しやすくする
 * 4) bind() で port に紐づけ
 * 5) listen() で待受け開始
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
    hints.ai_flags = AI_PASSIVE;      /* 待受け用（host を NULL にできる） */

    /* アドレス情報の決定 */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* デバッグ表示（実際に bind する port を数値で表示） */
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

    /* ソケットオプション：SO_REUSEADDR（再起動時に bind 失敗しづらくする） */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* bind：アドレス/ポートを割り当て */
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

/* 最大同時 “接続管理” 数（child[] 配列の最大要素数）
 *
 * 注意：
 * - ここでいう “child” は fork の子プロセスではなく、
 *   accept 済みの “接続ソケット FD” を入れる配列という意味
 */
#define MAX_CHILD (20)

/* accept + select によるイベントループ
 *
 * soc: listen ソケット FD
 *
 * アルゴリズム（繰り返し）：
 * 1) fd_set を作る（監視対象）
 *    - listen FD（新規接続用）
 *    - child[] に入っている接続 FD（既存クライアント用）
 * 2) select(width, &mask, NULL, NULL, timeout) で “読み込み可能” を待つ
 * 3) listen FD が ready なら accept し、child[] の空きに登録
 * 4) child[i] が ready なら send_recv(child[i], i) を1回呼ぶ
 *    - エラー/EOF なら close して child[i] = -1（空きに戻す）
 *
 * この方式の性質：
 * - 1回の select で複数 FD が ready になり得るため、その分だけ順に処理する
 * - ただし send_recv は “1回 recv して 1回 send するだけ” で短く終わる想定
 *   もし send_recv が長時間ブロックする/重い処理をすると、全体の応答が止まる
 */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    int child[MAX_CHILD];
    struct timeval timeout;
    struct sockaddr_storage from;
    int acc, child_no, width, i, count, pos, ret;
    socklen_t len;
    fd_set mask;

    /* child 配列の初期化：-1 を “空きスロット” とする */
    for (i = 0; i < MAX_CHILD; i++) {
        child[i] = -1;
    }

    /* child_no：配列上 “使う範囲” の上限（末尾のインデックス管理用）
     * - 空き(-1)が途中にあっても、child_no の範囲内を走査する設計
     */
    child_no = 0;

    for (;;) {
        /* 1) select 用の fd_set（mask）を構築する */
        FD_ZERO(&mask);

        /* listen FD を常に監視（新規接続が来たか） */
        FD_SET(soc, &mask);
        width = soc + 1;

        /* 既存接続（child[]）も監視に追加 */
        count = 0;
        for (i = 0; i < child_no; i++) {
            if (child[i] != -1) {
                FD_SET(child[i], &mask);

                /* select の第1引数は “最大FD+1” なので最大値を更新 */
                if (child[i] + 1 > width) {
                    width = child[i] + 1;
                }

                /* 接続数カウント（デバッグ表示用） */
                count++;
            }
        }

        (void) fprintf(stderr, "<<child count:%d>>\n", count);

        /* 2) select のタイムアウト設定（10秒）
         * - 10秒間何も起きなければ 0 が返る（タイムアウト）
         * - タイムアウト自体はこのコードでは “何もしない” が、
         *   監視ループが止まっていないことを確認しやすい
         */
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;

        /* 3) select：読み込み可能 FD を待つ
         * - 第2引数：readfds（mask）
         * - 第3/4引数：writefds/exceptfds は未使用
         */
        switch (select(width, (fd_set *) &mask, NULL, NULL, &timeout)) {
        case -1:
            /* select 自体のエラー */
            perror("select");
            break;

        case 0:
            /* タイムアウト（ready な FD は無し） */
            break;

        default:
            /* 4) ready がある */

            /* (a) listen FD が ready：新規接続の受付 */
            if (FD_ISSET(soc, &mask)) {
                len = (socklen_t) sizeof(from);

                /* accept：接続専用ソケット（acc）を得る */
                if ((acc = accept(soc, (struct sockaddr *)&from, &len)) == -1) {
                    if (errno != EINTR) {
                        perror("accept");
                    }
                } else {
                    /* 接続元の数値アドレス/ポートを表示 */
                    (void) getnameinfo((struct sockaddr *) &from, len,
                                       hbuf, sizeof(hbuf),
                                       sbuf, sizeof(sbuf),
                                       NI_NUMERICHOST | NI_NUMERICSERV);
                    (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

                    /* child[] の空きスロットを探す（-1 が空き） */
                    pos = -1;
                    for (i = 0; i < child_no; i++) {
                        if (child[i] == -1) {
                            pos = i;
                            break;
                        }
                    }

                    if (pos == -1) {
                        /* 空きが無い：child_no を伸ばして末尾に追加できるか検討 */
                        if (child_no + 1 >= MAX_CHILD) {
                            /* これ以上保持できない：接続を受けたが保持できないので即クローズ */
                            (void) fprintf(stderr, "child is full : cannot accept\n");
                            (void) close(acc);
                        } else {
                            /* 配列の “使用範囲” を拡張し、その末尾を使用 */
                            child_no++;
                            pos = child_no - 1;
                        }
                    }

                    if (pos != -1) {
                        /* accept 済みソケットを登録（以降 select の監視対象になる） */
                        child[pos] = acc;
                    }
                }
            }

            /* (b) 既存接続ソケットが ready：受信→応答を実行 */
            for (i = 0; i < child_no; i++) {
                if (child[i] != -1) {
                    if (FD_ISSET(child[i], &mask)) {
                        /* 送受信処理（1回分）
                         * - 正常なら 0
                         * - エラー/EOF なら -1
                         */
                        if ((ret = send_recv(child[i], i)) == -1) {
                            /* エラーまたは切断：クローズして空きに戻す */
                            (void) close(child[i]);
                            child[i] = -1;
                        }
                    }
                }
            }
            break;
        }
    }
}

/* サイズ指定文字列連結（strlcat 相当の安全版）
 *
 * dst  : 連結先バッファ
 * src  : 追加する文字列
 * size : dst バッファ全体のサイズ
 *
 * 目的：
 * - 応答文字列 ":OK\r\n" を付けるときにバッファオーバーフローしないようにする
 *
 * 戻り値：
 * - “連結しようとした結果の総文字数” を返す（strlcat の流儀）
 */
size_t
mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    /* dst の終端まで進める（最大 size まで） */
    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--)
        ;
    dlen = pd - dst;

    /* 既に終端に到達していて追加できない場合 */
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }

    /* dst の最終書き込み可能位置（終端 '\0' 分を残す） */
    pde = dst + size - 1;

    /* src を dst の末尾にコピー（溢れない範囲まで） */
    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    /* 残りを '\0' で埋めて確実に終端 */
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }

    /* src の長さ計算（ps を終端まで進める） */
    while (*ps++)
        ;

    return (dlen + (ps - src - 1));
}

/* 送受信（1回分）
 *
 * acc      : 接続済みソケット FD（child[i] の中身）
 * child_no : どの child スロットか（ログ表示用の番号）
 *
 * アルゴリズム：
 * 1) recv で受信
 *    - len == 0 → 相手が切断（EOF）→ -1
 *    - len < 0  → エラー → -1
 * 2) 受信文字列を表示（改行は切って見やすく）
 * 3) ":OK\r\n" を付けて send で返す
 *
 * 注意：
 * - TCP はストリームなので “1回 recv ＝ 1メッセージ” とは限らない
 * - send も部分送信があり得る（教材として単純化）
 * - buf[len]='\0' は len==sizeof(buf) の場合に境界外になる可能性がある
 *   → 安全化するなら recv(..., sizeof(buf)-1, ...) が定石
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

    /* 改行を見つけたら切る（ログを1行に揃える） */
    if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
        *ptr = '\0';
    }

    (void) fprintf(stderr, "[child%d]%s\n", child_no, buf);

    /* 応答文字列作成（安全連結） */
    (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
    len = strlen(buf);

    /* 応答送信（部分送信は未考慮） */
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
        (void) fprintf(stderr, "server2 port\n");
        return (EX_USAGE);
    }

    /* listen ソケット準備 */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    (void) fprintf(stderr, "ready for accept\n");

    /* イベントループ（select）開始 */
    accept_loop(soc);

    /* 実際は accept_loop は戻らない想定だが、形式上クローズ */
    (void) close(soc);
    return (EX_OK);
}

/*
 * 追加の改善ポイント（次のステップ）
 *
 * 1) width と count の扱い
 * - count++ を width 更新の条件の中に入れると正確でないので、
 *   “child が有効なら count++” のみにする方が自然（本コードは一応 count++ しているが位置注意）
 *
 * 2) buf 終端の安全化
 * - recv を sizeof(buf)-1 にして buf[len]='\0' を安全にする
 *
 * 3) 部分送信への対応
 * - send が返したバイト数が len より小さい可能性があるためループ送信が必要
 *
 * 4) select のスケール
 * - FD数が増えると select は遅くなる
 * - Linux なら epoll、BSD なら kqueue の導入が次の学習テーマ
 */
