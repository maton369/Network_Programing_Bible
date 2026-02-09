#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>                    /* POSIXスレッド API を使うために必要 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/*
 * server8: pthread_mutex による accept() の直列化 + スレッド並列処理
 *
 * 目的：
 *   - 1つの listening socket(soc) を複数スレッドで共有して accept() する。
 *   - ただし accept() の同時実行は避けたい（教材的に accept の競合を「見える化」したい）。
 *   - そこで pthread_mutex を「accept の前後だけ」取って、accept を直列化する。
 *
 * 全体像：
 *   main():
 *     - server_socket() で listening socket を作る
 *     - NUM_CHILD 本の accept_thread を作る（全員 soc を共有）
 *     - 親スレッドは監視用に g_lock_id を定期表示するだけ
 *
 *   accept_thread():
 *     - mutex を取る（= accept 権を取る）
 *     - accept(soc) で接続を受理
 *     - mutex を解放（次のスレッドに accept の機会を渡す）
 *     - 受け取った acc（接続済みソケット）で send_recv_loop() を実行して I/O を処理
 *
 * この方式のポイント：
 *   - accept は直列（mutex で 1 スレッドに限定）
 *   - 接続処理（send_recv_loop）は並列（接続ごとにスレッドが担当）
 *
 * 注意：
 *   - 本コードは教材/実験用の雰囲気が強い。
 *     実運用では「accept を直列化する必要があるか？」や「1接続=1スレッドはスケールするか？」
 *     を再検討することが多い（スレッドプール、epoll、SO_REUSEPORT 等）。
 */

/* accept スレッド数（並列 worker 数） */
#define NUM_CHILD       2

/*
 * accept() を直列化するための mutex。
 * - PTHREAD_MUTEX_INITIALIZER で静的初期化しているので main() で init は不要。
 * - lock/unlock の範囲を accept の部分だけに限定するのが重要（ロックを短くする）。
 */
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * 監視用：「今ロックを持っているスレッドID」を記録する（デバッグ/可視化目的）。
 * - -1: 誰も保持していない
 * - それ以外: pthread_self() の値（ただし pthread_t は本来整数とは限らない点に注意）
 *
 * 厳密性を求めるなら：
 *   - pthread_t をそのまま保持する
 *   - 表示は %lu + (unsigned long) pthread_self() のように整える（環境依存）
 */
int g_lock_id = -1;

/* --------------------------- サーバソケット準備 --------------------------- */

/*
 * server_socket(portnm)
 *   - TCP/IPv4 の listening socket を生成し、指定ポートで待ち受け開始する。
 *
 * 手順：
 *   1) getaddrinfo(NULL, portnm, ...) で bind 用アドレス（INADDR_ANY + port）を得る
 *   2) socket() でソケット生成
 *   3) setsockopt(SO_REUSEADDR) で TIME_WAIT 等による bind 失敗を緩和
 *   4) bind() でアドレス割り当て
 *   5) listen() で待受状態へ
 *   6) soc を返す
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

    /*
     * ai_family = AF_INET:
     *   - IPv4 に限定（教材として分かりやすい）
     * ai_socktype = SOCK_STREAM:
     *   - TCP
     * ai_flags = AI_PASSIVE:
     *   - hostnm = NULL のとき、bind 可能なワイルドカードアドレスを返す
     */
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    /* アドレス情報の決定（bind先アドレスを得る） */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /*
     * デバッグ表示用に、得られたアドレス/ポートを数値表記で表示する。
     * 例: port=55555
     */
    if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen,
                               nbuf, sizeof(nbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
        freeaddrinfo(res0);
        return (-1);
    }
    (void) fprintf(stderr, "port=%s\n", sbuf);

    /* ソケットの生成 */
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol))
        == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }

    /*
     * SO_REUSEADDR:
     *   - 前回のプロセス終了直後に同じポートで bind し直すと失敗しがち（TIME_WAIT）。
     *   - このオプションで緩和できる（ただし万能ではない）。
     */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* ソケットにアドレスを指定 */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /*
     * listen():
     *   - backlog に SOMAXCONN を指定（OSが許す最大級の待ち行列）
     */
    if (listen(soc, SOMAXCONN) == -1) {
        perror("listen");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    freeaddrinfo(res0);
    return (soc);
}

/* --------------------------- 送受信処理（1接続） --------------------------- */

/*
 * サイズ指定文字列連結 mystrlcat()
 *
 * 目的：
 *   - dst の末尾に src を連結するが、全体サイズ size を超えないようにする。
 *   - 典型的な strlcat 系の実装で、バッファオーバーフローを避ける教材。
 *
 * 返り値：
 *   - 連結しようとした結果の理想長（切り詰めが起きると size 以上になる可能性がある）
 */
size_t
mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    /* dst の終端を探しつつ、残りサイズを計算する */
    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--) ;
    dlen = (size_t)(pd - dst);

    /* 既にサイズを使い切っているなら、理想長だけ返す */
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }

    /* dst の書き込み可能最終位置（終端 '\0' の分を残す） */
    pde = dst + size - 1;

    /* src を連結（入りきる分だけ） */
    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    /* 残りを '\0' で埋める（確実に終端する） */
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }

    /* src の長さを数える（ps を末尾まで進める） */
    while (*ps++) ;
    return (dlen + (size_t)(ps - src - 1));
}

/*
 * send_recv_loop(acc)
 *
 * 役割：
 *   - 接続済みソケット acc から 1 行受信し、":OK\r\n" を付けて返信する。
 *   - クライアントが切断したら終了する。
 *
 * 実装のポイント：
 *   - recv() が 0 を返すと相手が閉じた（EOF）と判断できる。
 *   - 受信データは文字列として扱うので buf[len] = '\0' を入れる。
 *   - ログにスレッドID（pthread_self）を出して「どのスレッドが処理したか」を可視化。
 */
void
send_recv_loop(int acc)
{
    char buf[512], *ptr;
    ssize_t len;

    for (;;) {
        /* 受信 */
        if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
            /* ソケット受信エラー */
            perror("recv");
            break;
        }

        if (len == 0) {
            /* 相手が close した（EOF） */
            (void) fprintf(stderr, "<%d>recv:EOF\n", (int) pthread_self());
            break;
        }

        /* 受信内容を C 文字列にする（表示・加工のため） */
        buf[len] = '\0';

        /*
         * 改行コードを含むならそこで切る（ログを1行に収める）。
         * 例: "hello\r\n" -> "hello"
         */
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }

        /* 受信内容のログ */
        (void) fprintf(stderr, "<%d>[client]%s\n", (int) pthread_self(), buf);

        /* 応答文字列作成（元の文字列 + ":OK\r\n"） */
        (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
        len = (ssize_t) strlen(buf);

        /* 応答送信 */
        if ((len = send(acc, buf, (size_t) len, 0)) == -1) {
            perror("send");
            break;
        }
    }
}

/* --------------------------- accept 多重化（スレッド） --------------------------- */

/*
 * accept_thread(arg)
 *
 * 引数：
 *   arg は int*（listening socket の FD へのポインタ）を想定。
 *
 * アルゴリズム（ループ1回あたり）：
 *   1) mutex ロック（accept 権を獲得）
 *   2) accept(soc) で接続を受ける（ここがブロックしうる）
 *   3) mutex アンロック（次のスレッドに accept 機会を譲る）
 *   4) 受け取った acc で send_recv_loop()（接続処理はロック外で並列）
 *   5) acc を close して次へ
 *
 * なぜ mutex が必要？
 *   - 実際には OS は複数スレッドからの accept をある程度安全に扱えるが、
 *     教材として「accept の担当を 1 スレッドに限定する」ことで挙動を読みやすくする意図。
 *
 * 注意点：
 *   - この実装は「ロックを取ってから accept を呼ぶ」ため、
 *     そのスレッドが accept 待ちでブロックしている間、他スレッドは accept に入れない。
 *   - つまり accept の並列性をあえて潰している（意図的）。
 */
void *
accept_thread(void *arg)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;
    int acc, soc;
    socklen_t len;

    /* 引数（listening socket FD）を取り出す */
    soc = *(int *) arg;

    /* スレッドをデタッチ（join 不要にする） */
    pthread_detach(pthread_self());

    for (;;) {
        (void) fprintf(stderr, "<%d>ロック獲得開始\n", (int) pthread_self());

        /* mutex を取る（accept の順番待ち） */
        (void) pthread_mutex_lock(&g_lock);

        /* 監視用：今ロックを持っているスレッドIDを記録 */
        g_lock_id = (int) pthread_self();

        (void) fprintf(stderr, "<%d>ロック獲得！\n", (int) pthread_self());

        len = (socklen_t) sizeof(from);

        /*
         * accept():
         *   - 新しい接続が来るまでブロックする可能性がある。
         *   - このコードでは mutex を保持したまま accept するため、
         *     ここで止まっている間、他スレッドは lock で待つ（= accept が直列化される）。
         */
        if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
            /*
             * EINTR:
             *   - シグナル割り込みで accept が中断されることがある。
             *   - それ以外のエラーなら perror。
             */
            if (errno != EINTR) {
                perror("accept");
            }

            /* ロック解放（失敗でも必ず unlock する） */
            (void) fprintf(stderr, "<%d>ロック解放\n", (int) pthread_self());
            g_lock_id = -1;
            (void) pthread_mutex_unlock(&g_lock);

            /* 失敗したので次ループへ */
            continue;
        }

        /* accept 成功：接続元を表示 */
        (void) getnameinfo((struct sockaddr *) &from, len,
                           hbuf, sizeof(hbuf),
                           sbuf, sizeof(sbuf),
                           NI_NUMERICHOST | NI_NUMERICSERV);
        (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

        /*
         * 重要：ここでロックを解放する
         * - 受け付け処理（accept）だけを直列化したいので、接続処理はロック外へ出す。
         */
        (void) fprintf(stderr, "<%d>ロック解放\n", (int) pthread_self());
        g_lock_id = -1;
        (void) pthread_mutex_unlock(&g_lock);

        /* 送受信ループ（このスレッドが acc を担当して処理） */
        send_recv_loop(acc);

        /* 接続終了：accept ソケットを閉じる */
        (void) close(acc);
    }

    pthread_exit((void *) 0);
    /*NOT REACHED*/
    return ((void *) 0);
}

/* --------------------------- エントリポイント --------------------------- */

int
main(int argc, char *argv[])
{
    int i, soc;
    pthread_t thread_id;

    /* 引数にポート番号が指定されているか？ */
    if (argc <= 1) {
        (void) fprintf(stderr, "server8 port\n");
        return (EX_USAGE);
    }

    /* サーバソケットの準備 */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    /*
     * accept スレッドを NUM_CHILD 本生成する。
     * - 全スレッドに &soc を渡す（全員が同一 listening socket を共有する）。
     * - soc は main のスタック上にあるが、main は終了せずループし続けるため寿命的には問題になりにくい。
     *   ただし教材としては「soc をグローバルにする」方が誤解が少ないこともある。
     */
    for (i = 0; i < NUM_CHILD; i++) {
        if (pthread_create(&thread_id, NULL, accept_thread, (void *) &soc) != 0) {
            perror("pthread_create");
        } else {
            (void) fprintf(stderr,
                           "pthread_create:create:thread_id=%d\n",
                           (int) thread_id);
        }
    }

    (void) fprintf(stderr, "ready for accept\n");

    /*
     * 親スレッドは accept しない。
     * 代わりに 10 秒ごとに「誰がロックを持っているか」を表示して可視化する。
     */
    for (;;) {
        (void) sleep(10);
        (void) fprintf(stderr,
                       "<<%d>>ロック状態：%d\n",
                       (int) pthread_self(),
                       g_lock_id);
    }

    /*
     * 以下は到達しない（無限ループのため）。
     * 実運用ではシグナル等で抜けて close/destroy する設計にする。
     */
    (void) close(soc);
    (void) pthread_mutex_destroy(&g_lock);
    return (EX_OK);
}
