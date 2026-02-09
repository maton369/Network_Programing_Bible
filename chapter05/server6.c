#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>                    /* 追加：POSIXスレッド(pthread)を使うため */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/*
 * このプログラム（server6）の狙い：
 * - TCPサーバを立て、accept() で接続を受けたら
 *   「接続ごとに 1 スレッド」を生成して送受信処理を担当させる。
 *
 * つまり多重化のやり方が：
 * - server5：接続ごとに fork()（マルチプロセス）
 * - server6：接続ごとに pthread_create()（マルチスレッド）
 *
 * となっている。
 *
 * 全体アルゴリズム（ざっくり）：
 * 1) server_socket() で listen ソケット soc を作る
 * 2) accept_loop(soc) で accept を回す
 * 3) 接続が来たら acc（接続ソケット）を得る
 * 4) pthread_create で send_recv_thread を起動し、acc を渡す
 * 5) send_recv_thread は recv→ログ→応答(send) を繰り返し、切断で close(acc) して終了
 *
 * 注意（教材として重要）：
 * - スレッドは同一プロセスのメモリ空間を共有するため、共有データがある場合は排他制御が必要。
 *   このサンプルでは各スレッドが持つのは acc（接続FD）とローカルバッファ程度なので、
 *   競合が起こりにくい構成になっている。
 */

/* サーバソケットの準備 */
int
server_socket(const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    /*
     * getaddrinfo() に渡すヒント構造体を初期化する。
     * - ai_family   = AF_INET       : IPv4
     * - ai_socktype = SOCK_STREAM   : TCP
     * - ai_flags    = AI_PASSIVE    : bind() 用（サーバ側）で、
     *                                host=NULL のとき INADDR_ANY 相当になる
     */
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    /* アドレス情報の決定（portnm を元に bind 可能な sockaddr を得る） */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /*
     * getnameinfo() で「実際に bind しようとしている数値アドレス/ポート」を表示する。
     * NI_NUMERICHOST|NI_NUMERICSERV を付けることで逆引きせず数値で返す。
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

    /*
     * socket()：通信端点の作成
     * - res0 には getaddrinfo() が返した推奨パラメータが入っているので、それに従う。
     */
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol))
        == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }

    /*
     * setsockopt(SO_REUSEADDR)：
     * - サーバを再起動したとき、TIME_WAIT 等で bind できない状況を緩和するための定番オプション。
     */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /*
     * bind()：ソケットに「待受アドレス・ポート」を関連付ける。
     * これによりこのポートで接続を受けられるようになる。
     */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /*
     * listen()：受動オープン（待受状態）へ移行。
     * SOMAXCONN はOSが許す最大バックログ数を指定する。
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

/*
 * accept ループ（メインスレッド側の役割）
 *
 * 役割：
 * - soc（listen FD）で accept() を繰り返し、接続FD acc を得る
 * - 接続ごとに pthread_create() でワーカースレッドを作成し、acc を渡す
 * - 以降の送受信はワーカースレッドに任せ、自分は accept に戻る
 *
 * マルチスレッド多重化のポイント：
 * - 「同時接続数 ≒ スレッド数」になりやすい（接続ごとにスレッドを作るため）
 * - スレッドはプロセス内でメモリを共有する → ログや共有データを扱う場合はロックが必要
 * - 今回は各スレッドが独立した acc を扱うので単純
 */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;
    int acc;
    socklen_t len;

    /*
     * pthread_t はスレッドID（実体は実装依存の型）。
     * ここでは接続が来るたびに thread_id を上書きして使っているだけで、
     * join しない（detach する）設計なので保持し続ける必要がない。
     */
    pthread_t thread_id;

    /*
     * スレッド関数の前方宣言。
     * 本来はファイル先頭にプロトタイプ宣言しておく方が読みやすい。
     */
    void *send_recv_thread(void *arg);

    for (;;) {
        len = (socklen_t) sizeof(from);

        /*
         * accept()：接続受付
         * - 成功すると「接続ソケットFD（acc）」を返す（このFDで recv/send する）
         * - 失敗時 EINTR（シグナル割込み）なら再試行するのが基本
         */
        if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
            if (errno != EINTR) {
                perror("accept");
            }
        } else {
            /*
             * 接続元（クライアント）の IP/port をログに出す。
             * これにより接続が来たこと、どこから来たかが分かる。
             */
            (void) getnameinfo((struct sockaddr *) &from, len,
                               hbuf, sizeof(hbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV);
            (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

            /*
             * スレッド生成：
             * - 第1引数：生成されたスレッドIDの格納先
             * - 第2引数：属性（NULLならデフォルト）
             * - 第3引数：スレッド関数
             * - 第4引数：スレッド関数に渡す引数
             *
             * ここでは acc（int）を (void*) にキャストして渡している。
             * ※ 64bit環境では「int → void*」キャストは危険になり得る（幅が違う可能性）。
             *    教材としては malloc で int を確保してポインタで渡すか、
             *    intptr_t を使うのが安全。
             */
            if (pthread_create(&thread_id, NULL, send_recv_thread, (void *) acc)
                != 0) {
                perror("pthread_create");
                /*
                 * スレッド生成に失敗した場合、acc を誰も処理しないので close すべき。
                 * （現コードだと close が無いので FD リークの可能性がある点に注意）
                 */
                /* (void) close(acc);  ← 実運用ならここで閉じる */
            } else {
                /*
                 * thread_id の表示：
                 * pthread_t は単なる整数とは限らないので (int) キャスト表示は移植性が低い。
                 * ただ教材用途として「スレッドが増えている」ことを見せたい意図は分かる。
                 */
                (void) fprintf(stderr,
                               "pthread_create:create:thread_id=%d\n",
                               (int) thread_id);
            }

            /*
             * 親（accept ループ側）が acc を close しないのが、プロセス版（fork）との違い。
             * - fork 版では親は acc を閉じ、子が処理する（FDが複製されるから）
             * - スレッド版では同じプロセス内なので、close するとスレッド側も使えなくなる
             *   → acc の close はワーカースレッドが責任を持って行う
             */
        }
    }
}

/* サイズ指定文字列連結（strlcat 互換の自作） */
size_t
mystrlcat(char *dst, const char *src, size_t size)
{
    /*
     * 目的：
     * - dst の末尾に src を連結するが、バッファサイズ size を超えないようにする。
     * - 戻り値は「連結しようとした結果の総長さ（dst初期長 + src長）」で、
     *   size <= 戻り値 なら切り詰めが発生したと分かる、という strlcat の流儀。
     */
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    /* dst の終端を探しつつ、残り容量（lest）を減らしていく */
    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--) ;
    dlen = (size_t)(pd - dst);

    /* dst が size いっぱいで終端が無い場合：何も足せない */
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }

    /* pde は「最後に書ける位置」（終端 '\0' を置くため size-1） */
    pde = dst + size - 1;

    /* src を可能な限りコピー */
    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    /* 残りを '\0' で埋めて安全に終端 */
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }

    /* ps を src の終端まで進めて長さ計算（strlcat の返り値仕様） */
    while (*ps++) ;
    return (dlen + (size_t)(ps - src - 1));
}

/*
 * 送受信スレッド（接続ごとに 1 スレッド起動される）
 *
 * 役割：
 * - arg で渡された acc（接続FD）を使い、recv→応答(send) を繰り返す
 * - 切断（len==0）またはエラーで終了し、close(acc) してスレッド終了
 *
 * スレッドにする利点（教材観点）：
 * - accept ループがブロックせず、複数接続を並列にさばける
 * - fork より軽いことが多い（プロセス生成よりスレッド生成が軽量）
 *
 * 注意：
 * - 共有資源（共有バッファ、共有ログファイル、共有カウンタなど）を使うなら mutex が必要。
 * - ここでは標準エラー出力に複数スレッドが同時に書くため、ログが混ざる可能性はある。
 */
void *
send_recv_thread(void *arg)
{
    char buf[512], *ptr;
    ssize_t len;
    int acc;

    /*
     * スレッドをデタッチ：
     * - join しない（親が pthread_join で回収しない）設計にする。
     * - デタッチされたスレッドは終了時に OS が自動的に資源を回収する。
     *
     * つまりこのサンプルでは「スレッドの生死管理を簡単にする」ため detach している。
     */
    (void) pthread_detach(pthread_self());

    /*
     * 引数の取得：
     * - accept_loop から (void*)acc で渡されているので int に戻す。
     * - 64bit 環境では危険になり得る（前述）。
     */
    acc = (int) arg;

    for (;;) {
        /* 受信：TCPなので「受けた分だけ」返る（メッセージ境界は保証されない） */
        if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
            perror("recv");
            break;
        }
        if (len == 0) {
            /* 相手が close した（EOF） */
            (void) fprintf(stderr, "<%d>recv:EOF\n", (int) pthread_self());
            break;
        }

        /*
         * 文字列化：
         * - recv は生バイト列を返すので '\0' は付かない
         * - buf[len] = '\0' でC文字列化してログ出力しやすくする
         *
         * 注意：len が sizeof(buf) と同じ場合、buf[len] は範囲外になる。
         * 安全にするなら recv のサイズを sizeof(buf)-1 にすべき。
         */
        buf[len] = '\0';

        /* CR/LF を見つけたらそこを終端にして1行として扱う */
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }

        /* スレッドID付きログ：どの接続がどのスレッドで処理されているか分かる */
        (void) fprintf(stderr, "<%d>[client]%s\n", (int) pthread_self(), buf);

        /* 応答文字列作成：受信文字列に ":OK\r\n" を付ける */
        (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
        len = (ssize_t) strlen(buf);

        /* 応答送信 */
        if ((len = send(acc, buf, (size_t) len, 0)) == -1) {
            perror("send");
            break;
        }
    }

    /* スレッドが責任を持って接続FDを閉じる（accept側では閉じない） */
    (void) close(acc);

    /*
     * スレッド終了：
     * - pthread_exit で終了値を返せる（join するなら受け取れる）
     * - 今回 detach しているので戻り値は実質使われない
     */
    pthread_exit((void *) 0);

    /*NOT REACHED*/
    return ((void *) 0);
}

int
main(int argc, char *argv[])
{
    int soc;

    /* 引数にポート番号が指定されているか？ */
    if (argc <= 1) {
        (void) fprintf(stderr, "server6 port\n");
        return (EX_USAGE);
    }

    /* サーバソケットの準備（listen開始） */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    (void) fprintf(stderr, "ready for accept\n");

    /* アクセプトループ（ここから先は基本戻らない） */
    accept_loop(soc);

    /* 通常ここには来ないが、念のため */
    (void) close(soc);
    return (EX_OK);
}
