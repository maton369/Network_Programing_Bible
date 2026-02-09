/*
 * server4: epoll による多重化 TCP サーバ（Linux 専用）
 *
 * 目的：
 * - server2(select) / server3(poll) の流れを踏まえて、
 *   Linux の高性能 I/O 多重化 API である epoll を使って同様のサーバを書く例
 *
 * select/poll と epoll の “アルゴリズム上の違い”：
 * - select/poll：
 *   毎回「監視対象の集合（fd_set / pollfd 配列）」をユーザ側で作り直し、
 *   その集合全体を OS に渡して “全体走査” で ready を探す（O(N) になりやすい）
 *
 * - epoll：
 *   監視対象の登録を OS 側（カーネル）に保持させ、
 *   epoll_wait() で “ready になった FD だけ” を受け取る（スケールしやすい）
 *
 * このプログラムの全体アルゴリズム：
 * 1) server_socket(port) で listen ソケットを作る
 * 2) accept_loop(listen_fd) で epoll を初期化し、listen_fd を登録
 * 3) 以後ループ：
 *    - epoll_wait() で ready イベントを待つ
 *    - listen_fd が ready → accept → 接続 FD を epoll に登録
 *    - 接続 FD が ready → send_recv()（1回分）→ EOF/エラーなら epoll 解除して close
 *
 * 注意：
 * - この実装は “レベルトリガ（EPOLLIN）” の最小例（ET: Edge Trigger は使っていない）
 * - 受信・送信は簡略化されており、実運用向けの完全版ではない（部分送信等）
 */

#include <sys/epoll.h>                  /* epoll_create, epoll_ctl, epoll_wait */
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
 * portnm: 文字列ポート番号（例: "55555"）
 *
 * アルゴリズム：
 * - getaddrinfo(NULL, port, AI_PASSIVE) で待受けアドレスを得る
 * - socket → setsockopt(SO_REUSEADDR) → bind → listen
 */
int
server_socket(const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    /* hints を初期化（指定しない項目を 0 にする） */
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */
    hints.ai_flags = AI_PASSIVE;      /* 待受け用 */

    /* アドレス情報を解決 */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* 解決結果を数値で表示（学習用ログ） */
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

    /* 再起動時に bind しやすくする */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* bind（待受けポートへ割当） */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* listen（接続待ち状態へ） */
    if (listen(soc, SOMAXCONN) == -1) {
        perror("listen");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    freeaddrinfo(res0);
    return (soc);
}

/* 最大同時接続（epoll に登録する “接続FD” の上限を教材的に設ける）
 * NOTE:
 * - 実運用の epoll はもっと大きい値を扱えるが、ここは分かりやすさ優先
 */
#define MAX_CHILD (20)

/* epoll ベースの accept ループ（イベントループ）
 *
 * soc: listen ソケット FD
 *
 * epoll の使い方（最小手順）：
 * 1) epoll_create(...) で epoll インスタンス（epollfd）を作る
 * 2) epoll_ctl(ADD) で監視したい FD を登録する
 *    - まず listen FD（soc）を登録
 * 3) epoll_wait() で ready イベントを受け取る
 *    - 戻り値 nfds は “events 配列に詰められた ready 件数”
 * 4) ready になった FD ごとに処理する
 *    - listen FD → accept → 接続FDを epoll に ADD
 *    - 接続FD → recv/send → 終了なら epoll から DEL して close
 */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;
    int acc, count, i, epollfd, nfds, ret;
    socklen_t len;

    /* epoll_event:
     * - events : 監視したいイベント種別（EPOLLIN など）
     * - data   : “どのFDのイベントか” を識別するためのユーザデータ領域
     *           ここでは data.fd を使って FD を保持している
     */
    struct epoll_event ev;

    /* epoll_wait() が返す ready イベントの配列
     * NOTE:
     * - 第3引数の maxevents と合わせたサイズにするのが基本
     * - ここは MAX_CHILD になっているが、wait 側は MAX_CHILD+1 を渡しているのでズレがある
     *   （教材としては events[MAX_CHILD+1] に揃える方が安全）
     */
    struct epoll_event events[MAX_CHILD];

    /* epoll インスタンス生成
     * - 古い API では “サイズヒント” を渡す（Linux 2.6.8 以降はほぼ無視される）
     */
    if ((epollfd = epoll_create(MAX_CHILD + 1)) == -1) {
        perror("epoll_create");
        return;
    }

    /* listen FD を epoll に登録（新規接続イベントを監視） */
    ev.data.fd = soc;      /* events から “どのFDか” を判別できるように保存 */
    ev.events = EPOLLIN;   /* 読み込み可能（= accept できる接続が来た） */
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, soc, &ev) == -1) {
        perror("epoll_ctl");
        (void) close(epollfd);
        return;
    }

    /* 接続数のカウント（教材用の上限管理）
     * - epoll 自体は “child 配列” 不要だが、ここでは MAX_CHILD 制限のため count を持つ
     */
    count = 0;

    for (;;) {
        (void) fprintf(stderr, "<<child count:%d>>\n", count);

        /* epoll_wait：
         * - ready イベントが発生するまで待つ
         * - timeout は ms（ここでは 10 秒）
         * - 戻り値 nfds は events[] に入った件数
         *
         * NOTE:
         * - 第3引数 maxevents を MAX_CHILD+1 にしているのに
         *   events[] が MAX_CHILD だとオーバーランの危険がある
         *   （ここは教材として “配列サイズと maxevents を一致” させるのが重要）
         */
        switch ((nfds = epoll_wait(epollfd, events, MAX_CHILD + 1, 10 * 1000))) {
        case -1:
            perror("epoll_wait");
            break;

        case 0:
            /* タイムアウト：何もしないでループ継続 */
            break;

        default:
            /* ready なイベントを nfds 個処理する */
            for (i = 0; i < nfds; i++) {

                /* どのFDのイベントかを識別（data.fd を使う） */
                if (events[i].data.fd == soc) {
                    /* listen FD のイベント → accept */
                    len = (socklen_t) sizeof(from);

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

                        /* 接続上限チェック（教材用） */
                        if (count + 1 >= MAX_CHILD) {
                            (void) fprintf(stderr, "connection is full : cannot accept\n");
                            (void) close(acc);
                        } else {
                            /* 接続FDを epoll に登録（以後、このFDの受信イベントを待てる） */
                            ev.data.fd = acc;
                            ev.events = EPOLLIN;  /* 読み込み可能を監視 */
                            if (epoll_ctl(epollfd, EPOLL_CTL_ADD, acc, &ev) == -1) {
                                perror("epoll_ctl");
                                (void) close(acc);
                                (void) close(epollfd);
                                return;
                            }
                            count++;
                        }
                    }

                } else {
                    /* 接続FDのイベント → recv/send（1回分） */
                    if ((ret = send_recv(events[i].data.fd, events[i].data.fd)) == -1) {
                        /* EOF/エラー：監視解除してクローズ */

                        /* epoll から削除（DEL）
                         * NOTE:
                         * - DEL の第4引数（event）は通常無視されるが、NULL を渡す方が明確な実装も多い
                         */
                        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, events[i].data.fd, &ev) == -1) {
                            perror("epoll_ctl");
                            (void) close(acc);
                            (void) close(epollfd);
                            return;
                        }

                        (void) close(events[i].data.fd);
                        count--;
                    }
                }
            }
            break;
        }
    }

    /* 実際には戻らない想定だが形式上 */
    (void) close(epollfd);
}

/* サイズ指定文字列連結（strlcat 相当の安全版） */
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
 * child_no : ログ表示用番号（ここでは fd を渡している）
 *
 * アルゴリズム：
 * - recv で受信（エラー/EOFなら -1）
 * - 受信内容を表示し ":OK\r\n" を付けて send で返す
 *
 * 注意：
 * - buf[len] = '\0' は len==sizeof(buf) のとき境界外になり得る
 *   → recv を sizeof(buf)-1 にするのが安全
 * - send は部分送信があり得る（教材として簡略化）
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
     * NOTE: len==512 のとき境界外になり得る
     */
    buf[len] = '\0';

    /* 改行を除去してログを1行化 */
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

    /* 引数にポート番号が指定されているか？ */
    if (argc <= 1) {
        (void) fprintf(stderr, "server4 port\n");
        return (EX_USAGE);
    }

    /* listen ソケット準備 */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    (void) fprintf(stderr, "ready for accept\n");

    /* epoll ベースのイベントループ */
    accept_loop(soc);

    (void) close(soc);
    return (EX_OK);
}

/*
 * epoll 学習ポイントまとめ
 *
 * 1) 監視対象の登録は “最初に epoll_ctl(ADD)” で行い、毎回作り直さない
 *    - select/poll と違う最大のポイント
 *
 * 2) epoll_wait は “ready のみ” を events[] に詰めて返す
 *    - 大量FDでも、常に全FDを走査しなくてよい（スケールしやすい）
 *
 * 3) 今回は EPOLLIN のみのレベルトリガ
 *    - エッジトリガ（EPOLLET）では、ノンブロッキング + “読み切るまで recv” が必要になる
 */
