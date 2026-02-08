/*
 * TCPサーバ（IPv4 / SOCK_STREAM）の最小構成サンプル。
 *
 * 目的：
 * - 指定ポートで待ち受け (listen)
 * - クライアント接続を accept
 * - 1接続ごとに recv で1行受け取り、末尾に ":OK\r\n" を付けて send で返す
 *
 * アルゴリズム全体の流れ（超重要）：
 * 1) server_socket(port)
 *    - getaddrinfo で「待受に使うアドレス情報」を作る
 *    - socket でサーバソケット生成
 *    - setsockopt(SO_REUSEADDR) で再起動直後でも bind しやすくする
 *    - bind で「ポート番号」をソケットに割り当て
 *    - listen で「接続待ち」状態にする
 * 2) accept_loop(soc)
 *    - accept で接続を受け付ける（ここでクライアントごとの新しいソケット acc ができる）
 *    - send_recv_loop(acc) で送受信（この実装は逐次処理で、並列には捌かない）
 *    - close(acc) で接続終了
 *
 * 注意（ネットワークプログラミングの落とし穴）：
 * - recv / send は「要求した長さ全部が一度に来る/送れる」とは限らない（部分送受信があり得る）
 *   このコードは教材的に単純化しているため、send は1回で送れる前提に寄っている。
 * - TCPはストリームなので「1回の recv == 1メッセージ」とは限らない。
 *   改行区切りなどのプロトコル設計が必要。
 * - buf[len] = '\0' のためには、buf のサイズに対して len が最大 sizeof(buf)-1 である必要がある。
 *   しかし今の recv は sizeof(buf) まで受けるので len==512 の可能性があり、境界外書き込みになる。
 *   → 後述コメントで安全な形も示す。
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

/* forward declaration（Cでは関数を先に使うなら宣言が必要）
 * accept_loop から send_recv_loop を呼んでいるため、先に宣言しておく。
 */
void send_recv_loop(int acc);

/* サーバソケットの準備
 * portnm: 文字列のポート番号（例 "8080"）
 * 戻り値: 成功なら listen 済みソケットFD、失敗なら -1
 */
int
server_socket(const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    /* hints を 0 で初期化：
     * getaddrinfo の挙動は hints の指定で変わるため、未初期化だと事故る。
     */
    (void) memset(&hints, 0, sizeof(hints));

    /* ai_family = AF_INET：
     * IPv4 のみを使う指定。
     * IPv6も対応するなら AF_UNSPEC にするのが一般的。
     */
    hints.ai_family = AF_INET;

    /* ai_socktype = SOCK_STREAM：
     * TCP を使う指定（UDPなら SOCK_DGRAM）。
     */
    hints.ai_socktype = SOCK_STREAM;

    /* AI_PASSIVE：
     * サーバ側で待受するためのフラグ。
     * getaddrinfo の hostname に NULL を渡した場合、
     * 「任意のローカルアドレス(0.0.0.0)に bind できる」結果が得られやすい。
     */
    hints.ai_flags = AI_PASSIVE;

    /* アドレス情報の決定（DNS/サービス名解決も含む）
     * 第1引数 NULL + AI_PASSIVE => INADDR_ANY (0.0.0.0) での待受が基本になる。
     */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* getnameinfo で数値表記のホスト/ポートを表示する（デバッグ用）
     * NI_NUMERICHOST | NI_NUMERICSERV => 逆引きせず数値で返す
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

    /* ソケット生成
     * res0 は getaddrinfo が返した候補の先頭。
     * 本来は候補を順に試す実装が堅いが、ここでは1つ目を使っている。
     */
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol))
        == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }

    /* SO_REUSEADDR：
     * サーバ再起動直後に TIME_WAIT が残っていても bind が通りやすくなる。
     * 開発中のサーバではほぼ必須の定番オプション。
     */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* bind：
     * ソケットに「待受アドレス（ここでは主にポート番号）」を割り当てる。
     * 失敗原因例：権限不足(1024未満)、既に使用中、アドレス不正など。
     */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* listen：
     * このソケットを「接続受付待ち」の状態にする。
     * SOMAXCONN はOSが許す最大バックログ（保留接続キュー）を意味する。
     */
    if (listen(soc, SOMAXCONN) == -1) {
        perror("listen");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* getaddrinfo の結果を解放（重要：メモリリーク回避） */
    freeaddrinfo(res0);

    /* listen 済みサーバソケットFDを返す */
    return (soc);
}

/* アクセプトループ
 * soc: listen 済みのサーバソケットFD
 *
 * ここは「クライアントが来るまで待つ」ブロッキングポイント。
 * accept すると、クライアントとの通信専用の新FD acc が作られる。
 */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from; /* IPv4/IPv6どちらでも入れられる汎用構造体 */
    int acc;
    socklen_t len;

    for (;;) {
        len = (socklen_t) sizeof(from);

        /* 接続受付：
         * - 成功すると「通信専用ソケット」acc が返る
         * - soc 自体は引き続き待受専用で使い続ける
         */
        if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {

            /* accept はシグナル割り込みで EINTR になることがある。
             * EINTR の場合は単にループ継続が定番。
             */
            if (errno != EINTR) {
                perror("accept");
            }
        } else {

            /* 接続元（クライアント）のIP:PORTを文字列化して表示 */
            (void) getnameinfo((struct sockaddr *) &from, len,
                               hbuf, sizeof(hbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV);
            (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

            /* 送受信ループ（この実装は「1接続を最後まで処理してから次を accept」する逐次型）
             * 同時接続を捌くには：
             * - fork で子プロセスに処理させる
             * - pthread などでスレッド化
             * - select/poll/epoll/kqueue などで多重化
             * - non-blocking + イベント駆動
             * が一般的。
             */
            send_recv_loop(acc);

            /* クライアントとの通信ソケットをクローズ（重要） */
            (void) close(acc);
            acc = 0;
        }
    }
}

/* サイズ指定文字列連結（strlcat 相当）
 *
 * 目的：
 * - dst のバッファサイズ size を超えないように src を末尾に連結する
 * - 返り値は「連結しようとした後の総文字数」（切り詰めが起きたかの判断に使える）
 *
 * なぜ必要？
 * - strcat はバッファ境界を知らないので危険（バッファオーバーフロー）
 * - snprintf を使うのが一般的だが、教材として strlcat の実装を載せている形
 */
size_t
mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;
    
    /* dst の現在長を数える（最大 size まで）
     * lest は残りバイト数カウンタ。
     */
    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--);
    dlen = (size_t)(pd - dst);

    /* dst が size いっぱいで終端していない場合（= 既に満杯） */
    if (size - dlen == 0) {
        return (dlen + strlen(src)); /* 連結したかった長さを返す（切り詰め検出用） */
    }

    /* pde は「書き込み可能な最後の位置（終端 '\0' 用に1バイト残す）」 */
    pde = dst + size - 1;

    /* src を詰められるだけコピー */
    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    /* 必ず終端NULで埋める（pd〜pde） */
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }

    /* ps を最後まで進めて「本来の src 長」を確定する */
    while (*ps++);

    /* dst元の長さ + srcの長さ（実際に入った長さではなく、入れようとした長さ） */
    return (dlen + (size_t)(ps - src - 1));
}

/* 送受信ループ
 * acc: accept で得られた「クライアントと通信する」ソケットFD
 *
 * プロトコル（このコードが暗黙に期待しているもの）：
 * - クライアントが「1行」を送る（末尾に \r\n か \n が来る想定）
 * - サーバは受け取った文字列に ":OK\r\n" を付けて返す
 *
 * TCPの性質上、recv 1回で1行が来る保証はないが、簡略化している。
 */
void
send_recv_loop(int acc)
{
    char buf[512], *ptr;
    ssize_t len;

    for (;;) {

        /* 受信：
         * - 第4引数 flags=0 で通常受信
         * - 戻り値 len:
         *   >0: 受け取ったバイト数
         *    0: 相手が orderly shutdown（接続を閉じた）= EOF
         *   -1: エラー（errno参照）
         *
         * 注意：ここはブロッキングrecv。
         * 相手が送らない限りここで止まる。
         */
        if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
            /* エラー */
            perror("recv");
            break;
        }

        if (len == 0) {
            /* 相手が接続を閉じた（EOF） */
            (void) fprintf(stderr, "recv:EOF\n");
            break;
        }

        /* 文字列化・表示
         *
         * !!! 重要なバグ注意 !!!
         * recv は最大 sizeof(buf) バイト受け取る可能性がある。
         * そのとき len == 512 となり、buf[512] = '\0' は境界外アクセス。
         *
         * 安全にするなら：
         * - recv のサイズを sizeof(buf)-1 にする
         *   例: recv(acc, buf, sizeof(buf)-1, 0)
         * が定番。
         */
        buf[len] = '\0';

        /* 改行（\r か \n）があればそこで文字列を切る
         * strpbrk は「指定文字集合のどれかに最初にマッチする位置」を返す
         */
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }

        (void) fprintf(stderr, "[client]%s\n", buf);

        /* 応答文字列作成：
         * 受け取った buf の末尾に ":OK\r\n" を付ける
         * mystrlcat を使うことでバッファサイズを超えないようにする
         */
        (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));

        /* send するサイズは NUL までの長さ（strlen） */
        len = (ssize_t) strlen(buf);

        /* 応答送信：
         * send も部分送信があり得る（戻り値が要求サイズより小さい可能性）。
         * このコードは教材的に1回で送れる前提に寄っている。
         * 堅牢化するなら「全部送るまでループ」が必要。
         */
        if ((len = send(acc, buf, (size_t) len, 0)) == -1) {
            /* エラー */
            perror("send");
            break;
        }
    }
}

int
main(int argc, char *argv[])
{
    int soc;

    /* 引数にポート番号が指定されているか？
     * 例: ./server 8080
     */
    if (argc <= 1) {
        (void) fprintf(stderr, "server port\n");
        return (EX_USAGE);
    }

    /* サーバソケットの準備（listen済み） */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    (void) fprintf(stderr, "ready for accept\n");

    /* 接続受付ループ（無限ループ） */
    accept_loop(soc);

    /* ここには基本到達しない（accept_loopが無限のため） */
    (void) close(soc);
    return (EX_OK);
}

/*
 * 追加の改善ポイント（学習用メモ）：
 *
 * 1) buf境界外アクセスの修正
 *    recv(acc, buf, sizeof(buf)-1, 0) にするのが最低限。
 *
 * 2) 部分送信への対応
 *    send は一度に全部送れない可能性があるため、全量送るまで繰り返す send_all を作る。
 *
 * 3) 同時接続対応
 *    - fork: accept後に fork して子で send_recv_loop、親は accept 続行
 *    - select/poll/epoll/kqueue: 1スレッドで多数接続を多重化
 *
 * 4) タイムアウト
 *    - setsockopt(SO_RCVTIMEO / SO_SNDTIMEO)
 *    - もしくは select/poll で待ち時間制御
 */
