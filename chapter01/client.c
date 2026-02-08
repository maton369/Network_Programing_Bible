/*
 * TCPクライアント（IPv4 / SOCK_STREAM）のサンプル。
 *
 * 目的：
 * - 指定したサーバ（host:port）へ connect する
 * - 標準入力（キーボード）から1行読んでサーバへ送信する
 * - サーバから届いたデータを受信して画面に表示する
 *
 * 本コードの最大のポイント：
 * - select() を使って「ソケット受信」と「標準入力」を同時に待てるようにしている
 *   （I/O多重化：イベント駆動の超基本）
 * - これにより、ブロッキングな recv() や fgets() のどちらかで固まるのを避け、
 *   “入力もできるし受信も表示できる” 対話型クライアントになる。
 *
 * アルゴリズム全体の流れ（超重要）：
 * 1) client_socket(host, port)
 *    - getaddrinfo で接続先アドレス（IPとポート）を解決
 *    - socket() でTCPソケット作成
 *    - connect() でサーバへ接続
 * 2) send_recv_loop(soc)
 *    - select() で (A)ソケット, (B)標準入力 のどちらが読めるか待つ（最大1秒）
 *    - ソケットが読めるなら recv() して表示
 *    - 標準入力が読めるなら fgets() して send() で送信
 * 3) close(soc) で終了
 *
 * 注意（ネットワークの落とし穴）：
 * - recv / send は「部分送受信」が起こり得る（特に send は全部送れない可能性）
 * - TCPはストリーム：1回recvで1メッセージが来る保証はない（分割・結合され得る）
 * - buf[len] = '\0' のためには len <= sizeof(buf)-1 が必要
 *   しかし現状 recv のサイズが sizeof(buf) なので len==512 で境界外書き込みの可能性がある
 *   → 安全化するなら recv のサイズを sizeof(buf)-1 にするのが定番
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

/* サーバにソケット接続（クライアント側の接続準備）
 *
 * hostnm: 接続先ホスト名（例 "localhost", "example.com", "192.168.0.10"）
 * portnm: 接続先ポート番号（文字列）（例 "8080"）
 * 戻り値: 成功なら connect 済みソケットFD、失敗なら -1
 */
int
client_socket(const char *hostnm, const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, errcode;
    
    /* hints をゼロクリア（未初期化のゴミがあると getaddrinfo の挙動が壊れる） */
    (void) memset(&hints, 0, sizeof(hints));

    /* IPv4 のみ使用（IPv6も対応するなら AF_UNSPEC） */
    hints.ai_family = AF_INET;

    /* TCP を使用（UDPなら SOCK_DGRAM） */
    hints.ai_socktype = SOCK_STREAM;

    /* アドレス情報の決定：
     * hostnm/portnm を DNS/サービス名解決し、接続先候補（addrinfoのリスト）を得る
     * 実装としては res0 の1個目だけを使っているが、堅牢にするなら候補を順に試す。
     */
    if ((errcode = getaddrinfo(hostnm, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* デバッグ出力用：
     * 接続先アドレスを「数値表記」で表示（逆引きを避ける）
     */
    if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen,
                               nbuf, sizeof(nbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
        freeaddrinfo(res0);
        return (-1);
    }
    (void) fprintf(stderr, "addr=%s\n", nbuf);
    (void) fprintf(stderr, "port=%s\n", sbuf);

    /* ソケット生成（まだ未接続のTCPソケット） */
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol))
        == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }

    /* connect：
     * TCPの3-way handshake を行い、サーバへ接続する
     * 失敗原因例：サーバが起動していない、ポートが開いていない、DNS失敗など
     */
    if (connect(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("connect");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* getaddrinfo の結果を解放 */
    freeaddrinfo(res0);

    /* connect 済みFDを返す */
    return (soc);
}

/* 送受信処理（select によるI/O多重化）
 *
 * soc: connect 済みソケットFD
 *
 * この関数がやっていること（設計の要点）：
 * - select() により「読めるようになったFD」を検出する
 * - 対象FDは2つ：
 *   (1) soc（サーバからデータが届く）
 *   (2) 0（標準入力：ユーザがEnterを押して1行入力できる）
 *
 * select の利点：
 * - recv がブロックして固まることを避けつつ入力も受け付けられる
 * - 1スレッドで複数I/Oを扱うイベント駆動の基本
 */
void
send_recv_loop(int soc)
{
    char buf[512];
    struct timeval timeout;
    int end, width;
    ssize_t len;
    fd_set  mask, ready;

    /* select()用マスク（監視対象FD集合）の初期化 */
    FD_ZERO(&mask);

    /* 監視対象にソケットFDを追加
     * FD_SET は「そのFDを監視集合に入れる」マクロ
     */
    FD_SET(soc, &mask);

    /* 監視対象に標準入力（fd=0）を追加
     * UNIXでは標準入力=0, 標準出力=1, 標準エラー=2 が慣習
     */
    FD_SET(0, &mask);

    /* select の第1引数 width は「監視対象FDの最大値+1」
     * ここでは soc が最大だと仮定して soc+1 を入れている
     * （複数FDなら max(fd)+1 を計算する）
     */
    width = soc + 1;

    /* 送受信ループ
     * end=1 になったら終了
     */
    for (end = 0;; ) {

        /* ready は select から戻ると「実際に準備できたFD集合」に上書きされるため、
         * 毎回、監視集合 mask をコピーして渡す必要がある（重要）
         */
        ready = mask;

        /* タイムアウト値（最大1秒待つ）
         * - 1秒以内に何も起きなければ select は 0 を返す（タイムアウト）
         * - タイムアウトを入れる理由：
         *   無限待ちにせず、ループが定期的に戻って制御できるようにするため。
         */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        /* select の戻り値：
         * -1: エラー（errno参照）
         *  0: タイムアウト（何も起きていない）
         * >0: どれかのFDがレディになった（読み取り可能など）
         *
         * 引数：
         * - readfds = &ready : 読めるFDを知りたい
         * - writefds = NULL  : 書けるFDは今回は不要
         * - exceptfds = NULL : 例外状態は今回は不要
         * - timeout = &timeout : 待ち時間
         */
        switch (select(width, (fd_set *) &ready, NULL, NULL, &timeout)) {
        case -1:
            /* エラー
             * EINTR（シグナル割り込み）なら継続するのが一般的だが、
             * このコードは単純に perror だけしてループ継続する形になっている。
             */
            perror("select");
            break;

        case 0:
            /* タイムアウト：何も起きていないので次のループへ */
            break;

        default:
            /* レディ有り */

            /* (A) ソケットがレディなら、サーバからのデータを受信する */
            if (FD_ISSET(soc, &ready)) {

                /* 受信：
                 * 注意：ここも部分受信があり得る。
                 * また、buf[len]='\0' の境界問題があるので、本来は sizeof(buf)-1 で recv するのが安全。
                 */
                if ((len = recv(soc, buf, sizeof(buf), 0)) == -1) {
                    /* エラー */
                    perror("recv");
                    end = 1;
                    break;
                }

                if (len == 0) {
                    /* サーバが接続を閉じた（EOF） */
                    (void) fprintf(stderr, "recv:EOF\n");
                    end = 1;
                    break;
                }

                /* 文字列化・表示
                 *
                 * !!! 注意：len==512 の場合、buf[512]='\0' は境界外アクセス !!!
                 * 安全化の定番は recv の第3引数を sizeof(buf)-1 にすること。
                 */
                buf[len] = '\0';

                /* 表示：
                 * サーバ側が \r\n を付けて送ってくる想定なので、そのまま表示している
                 */
                (void) printf("> %s", buf);
            }

            /* (B) 標準入力がレディなら、ユーザ入力を読み取ってサーバへ送る */
            if (FD_ISSET(0, &ready)) {

                /* 1行読み込み（改行まで）
                 * fgets は改行を含んだまま buf に入るのが通常挙動
                 */
                (void) fgets(buf, sizeof(buf), stdin);

                /* EOF（Ctrl+Dなど）なら終了 */
                if (feof(stdin)) {
                    end = 1;
                    break;
                }

                /* 送信：
                 * send は部分送信の可能性があるが、このコードは単純化して1回で送れる前提。
                 * 実運用寄りにするなら「全量送るまでループ」する send_all が必要。
                 */
                if ((len = send(soc, buf, strlen(buf), 0)) == -1) {
                    /* エラー */
                    perror("send");
                    end = 1;
                    break;
                }
            }
            break;
        }

        /* 終了フラグが立っていたら抜ける */
        if (end) {
            break;
        }
    }
}

int
main(int argc, char *argv[])
{
    int soc;

    /* 引数にホスト名・ポート番号が指定されているか？
     * 例: ./client localhost 8080
     */
    if (argc <= 2) {
        (void) fprintf(stderr, "client server-host port\n");
        return (EX_USAGE);
    }

    /* サーバにソケット接続 */
    if ((soc = client_socket(argv[1], argv[2])) == -1) {
        (void) fprintf(stderr, "client_socket():error\n");
        return (EX_UNAVAILABLE);
    }

    /* 送受信処理（入力と受信を同時に扱う） */
    send_recv_loop(soc);

    /* ソケットクローズ */
    (void) close(soc);

    return (EX_OK);
}

/*
 * 追加の改善ポイント（学習用メモ）
 *
 * 1) buf終端の境界問題
 *    recv(soc, buf, sizeof(buf)-1, 0) に変えるのが最低限の安全化。
 *
 * 2) send の部分送信対応
 *    send_all を実装し、全量送るまで send を繰り返す。
 *
 * 3) fgets の戻り値チェック
 *    feof だけでなく NULL 戻り（エラー）も扱うと堅牢。
 *
 * 4) width の計算
 *    監視FDが増えた場合は max(fd)+1 を毎回計算するか、管理する。
 *
 * 5) 標準入力 fd=0 の可搬性
 *    POSIX環境では問題ないが、より一般化するなら fileno(stdin) を使う手もある。
 */
