/*
 * TCPサーバ（IPv4 / SOCK_STREAM）のサンプル：bind先のホスト名（IPアドレス）を指定できる版。
 *
 * 目的：
 * - 引数で指定された (address, port) に対して bind して待ち受ける
 *   例）./server1 127.0.0.1 55555
 * - accept で接続を受け付け、1接続ずつ recv → ":OK\r\n" 付与 → send を行う
 *
 * 前の server_socket(NULL, port) との違い（重要）：
 * - 前の版：host=NULL + AI_PASSIVE → 0.0.0.0:port（全NICで待受）
 * - 今の版：host=argv[1] を getaddrinfo に渡す → 指定したアドレスにだけ bind
 *
 * 例：
 * - 127.0.0.1 を指定すると、ループバック経由の接続しか受け付けない（外部からは繋がらない）
 * - 192.168.x.y を指定すると、そのLAN内からの接続を受け付ける（そのIPに来る接続のみ）
 * - 0.0.0.0 を指定すると、実質「全てのインタフェースで待受」になる（環境による）
 *
 * アルゴリズム全体の流れ：
 * 1) server_socket_by_hostname(host, port)
 *    - getaddrinfo(host, port) で bind 用アドレスを確定
 *    - socket() でサーバソケット生成
 *    - setsockopt(SO_REUSEADDR) で再起動しやすくする
 *    - bind() で指定アドレスに束縛
 *    - listen() で接続待ち状態へ
 * 2) accept_loop(listen_fd)
 *    - accept() で接続を1つ受ける
 *    - send_recv_loop(conn_fd) で送受信して返す
 *    - close(conn_fd)
 * 3) （通常到達しないが）close(listen_fd)
 *
 * 注意（落とし穴）：
 * - recv のサイズが sizeof(buf) のため、len==512 になり得て buf[len]='\0' が境界外アクセス
 *   → 安全化するなら recv(..., sizeof(buf)-1, ...) が定番
 * - send は部分送信があり得る（本コードは単純化して1回で送れる前提）
 * - TCPはストリームであり、1回の recv == 1行とは限らない（行バッファリングが必要になり得る）
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

/* forward declaration
 * accept_loop から send_recv_loop を呼ぶので、前方宣言しておく（Cでは重要）。
 */
void send_recv_loop(int acc);

/* サーバソケットの準備（host指定版）
 *
 * hostnm: bind したいアドレス（例 "127.0.0.1" / "192.168.0.10" / "0.0.0.0" など）
 * portnm: 待受ポート番号（文字列）（例 "55555"）
 * 戻り値: 成功なら listen 済みソケットFD、失敗なら -1
 *
 * この関数がやっていること（アルゴリズム的には「listen socket を作る」）：
 * - getaddrinfo で bind に使う sockaddr を取得
 * - socket 生成 → setsockopt → bind → listen
 */
int
server_socket_by_hostname(const char *hostnm, const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    /* hints をゼロクリア：
     * getaddrinfo のヒントを明示するため、未初期化を避ける。
     */
    (void) memset(&hints, 0, sizeof(hints));

    /* IPv4のみ対応（IPv6含めるなら AF_UNSPEC） */
    hints.ai_family = AF_INET;

    /* TCP */
    hints.ai_socktype = SOCK_STREAM;

    /* AI_PASSIVE：
     * 通常は「サーバの待受用」に使うフラグ。
     * ただし今回は hostnm を明示的に渡しているため、
     * NULL を渡すケースほど「INADDR_ANY」に寄せる効果は強くない。
     * （教材として“待受用”の意図を示すため入れていると解釈できる）
     */
    hints.ai_flags = AI_PASSIVE;

    /* アドレス情報の決定：
     * hostnm:portnm を sockaddr に変換する。
     * ここでは res0 の先頭要素だけを使っているが、堅牢にするなら候補を順に試す。
     */
    if ((errcode = getaddrinfo(hostnm, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* デバッグ表示（数値表記で addr/port を確認できる） */
    if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen,
                               nbuf, sizeof(nbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
        freeaddrinfo(res0);
        return (-1);
    }
    (void) fprintf(stderr, "addr=%s\nport=%s\n", nbuf, sbuf);

    /* ソケット生成（まだ未bind） */
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol))
        == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }

    /* SO_REUSEADDR：
     * 再起動時に bind が通りやすくなる（TIME_WAIT 等で詰まるのを回避）
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
     * 指定した host:port にソケットを束縛（待受ける“口”を確定）
     */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* listen：
     * 受け付けキュー（バックログ）を用意して接続待ち状態にする
     */
    if (listen(soc, SOMAXCONN) == -1) {
        perror("listen");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* アドレス情報の解放（メモリリーク防止） */
    freeaddrinfo(res0);

    /* listen済みFDを返す */
    return (soc);
}

/* アクセプトループ（逐次型）
 *
 * soc: listen 済みサーバソケットFD
 *
 * アルゴリズム：
 * - accept で接続が来るまで待つ
 * - 接続が来たら conn_fd（acc）を得る
 * - その conn_fd を使って send_recv_loop で送受信する
 * - conn_fd を close
 * - 次の accept へ
 *
 * 特徴：
 * - 1接続ずつ処理するため、同時接続を捌けない（逐次処理）
 * - 同時接続対応には fork/thread/I/O多重化が必要
 */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from; /* 接続元アドレス（IPv4/IPv6どちらでも入る型） */
    int acc;
    socklen_t len;

    for (;;) {
        len = (socklen_t) sizeof(from);

        /* 接続受付：
         * - 成功すると通信専用ソケットFD（acc）が返る
         * - listen用 soc は引き続き accept 用に保持される
         */
        if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
            /* シグナル割り込みで EINTR になることがあるので、その場合は無視して続行 */
            if (errno != EINTR) {
                perror("accept");
            }
        } else {
            /* 接続元をログ出力（数値表記） */
            (void) getnameinfo((struct sockaddr *) &from, len,
                               hbuf, sizeof(hbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV);
            (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

            /* 送受信ループ（この接続が終わるまで次の accept に行かない） */
            send_recv_loop(acc);

            /* 接続用ソケットをクローズ */
            (void) close(acc);
            acc = 0;
        }
    }
}

/* サイズ指定文字列連結（strlcat相当）
 *
 * 目的：
 * - dst バッファのサイズ size を超えないように src を末尾に連結する
 * - バッファオーバーフローを起こしにくい連結手法
 *
 * 返り値：
 * - 「本来連結したかった場合の合計長」を返す（切り詰めの検出に使える）
 */
size_t
mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;
    
    /* dst の終端を探す（最大 size まで） */
    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--);
    dlen = (size_t)(pd - dst);

    /* dst がすでに size いっぱいなら、何も追加できない */
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }

    /* 書き込み可能な最後（終端NULのために1バイト残す） */
    pde = dst + size - 1;

    /* src を入るだけコピー */
    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    /* 終端NULで埋める */
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }

    /* src 長計算のため ps を最後まで進める */
    while (*ps++);

    return (dlen + (size_t)(ps - src - 1));
}

/* 送受信ループ（エコー＋OK応答）
 *
 * acc: accept で得た「接続専用」ソケットFD
 *
 * アルゴリズム：
 * - recv でデータ受信
 * - 改行（\r または \n）までを1行として扱いログ表示
 * - 受信文字列に ":OK\r\n" を付けて返信
 * - クライアントが切断したら終了
 *
 * 注意：
 * - TCPストリームなので「1 recv = 1行」と限らない（教材的に単純化）
 * - recv のサイズ指定と NUL終端に境界問題がある（後述）
 */
void
send_recv_loop(int acc)
{
    char buf[512], *ptr;
    ssize_t len;

    for (;;) {

        /* 受信（ブロッキング）
         * 戻り値：
         *  >0: 受信バイト数
         *   0: 相手が接続を閉じた（EOF）
         *  -1: エラー
         */
        if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
            perror("recv");
            break;
        }

        if (len == 0) {
            (void) fprintf(stderr, "recv:EOF\n");
            break;
        }

        /* 文字列化
         *
         * !!! 注意：len==512 の場合、buf[512]='\0' は境界外書き込み !!!
         * 安全化するなら recv の第3引数を sizeof(buf)-1 にするのが定番：
         *   recv(acc, buf, sizeof(buf)-1, 0)
         */
        buf[len] = '\0';

        /* 改行があればそこで切る（1行として扱う） */
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }

        (void) fprintf(stderr, "[client]%s\n", buf);

        /* 応答文字列作成（末尾にOKを付加） */
        (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));

        len = (ssize_t) strlen(buf);

        /* 応答送信
         * 注意：send は部分送信があり得るが、このコードは単純化して1回で送れる前提
         * 堅牢化するなら送信残量が0になるまでsendを繰り返す。
         */
        if ((len = send(acc, buf, (size_t) len, 0)) == -1) {
            perror("send");
            break;
        }
    }
}

int
main(int argc, char *argv[])
{
    int soc;

    /* 引数にIPアドレス・ポート番号が指定されているか？
     * 例: ./server1 127.0.0.1 55555
     */
    if (argc <= 2) {
        (void) fprintf(stderr, "server1 address port\n");
        return (EX_USAGE);
    }

    /* サーバソケットの準備（指定アドレスに bind する版） */
    if ((soc = server_socket_by_hostname(argv[1], argv[2])) == -1) {
        (void) fprintf(stderr, "server_socket_by_hostname(%s,%s):error\n",
                       argv[1], argv[2]);
        return (EX_UNAVAILABLE);
    }

    (void) fprintf(stderr, "ready for accept\n");

    /* アクセプトループ（無限） */
    accept_loop(soc);

    /* 通常到達しないが、念のためクローズ */
    (void) close(soc);

    return (EX_OK);
}

/*
 * 追加の改善ポイント（学習用メモ）
 *
 * 1) bind先指定の意義を明確にする
 * - 127.0.0.1: 外部から接続不可（ローカル限定）
 * - 192.168.x.y: LAN内から接続可（そのIPに来る接続のみ）
 * - 0.0.0.0: 全インタフェース待受（実質 “外部からも可”）
 *
 * 2) アドレス候補を順に試す
 * getaddrinfo の res0 はリストなので、通常は ai_next を辿って bind 成功する候補を探す。
 *
 * 3) バッファ境界修正
 * recv(..., sizeof(buf)-1, ...) にして NUL終端用に1バイト残す。
 *
 * 4) 部分送信対応
 * send_all を作り、全量送るまで send を繰り返す。
 *
 * 5) 同時接続対応
 * fork/thread/select/poll/epoll 等で並列に捌ける設計へ拡張する。
 */
