#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <arpa/telnet.h>                /* Telnet の IAC/WILL/WONT/DO/DONT など */
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

/*
 * このプログラムは「超簡易 telnet クライアント」の例である。
 *
 * ポイントは大きく 3 つ:
 *
 * 1) TCP 接続: getaddrinfo() で解決 → socket() → connect()
 * 2) I/O 多重化: select() で「標準入力(キーボード)」と「ソケット受信」を同時に待つ
 * 3) Telnet の制御シーケンス処理: IAC(Interpret As Command) を検出したら WONT で否定応答する
 *
 * さらに、端末を raw モードにして 1 文字ずつ即時送信する（行入力ではなくキー入力をそのまま送る）。
 */

/* ソケット（接続済み TCP ソケット）をグローバルに保持 */
int g_soc = -1;

/*
 * 終了フラグ
 * - シグナルハンドラから書き換えられるため、volatile sig_atomic_t を使う
 * - sig_atomic_t は「シグナルハンドラ内で安全に読み書きできる最小単位」を意図している
 */
volatile sig_atomic_t g_end = 0;

/*
 * サーバに TCP 接続する
 *
 * hostnm: 接続先ホスト名（例: "example.com"）
 * portnm: サービス名 or ポート番号文字列（例: "telnet" や "23"）
 *
 * 戻り値:
 *   成功: 接続済みソケット fd
 *   失敗: -1
 */
int
client_socket(const char *hostnm, const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, errcode;

    /* getaddrinfo() のヒント（条件）をゼロクリアしてから設定する */
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* IPv4 に固定（学習用。IPv6 対応なら AF_UNSPEC） */
    hints.ai_socktype = SOCK_STREAM;    /* TCP */

    /*
     * アドレス情報の決定:
     * - hostnm, portnm から接続先候補（sockaddr のリスト）を得る
     * - 本コードは res0 の先頭だけを使う（厳密には候補を順に試すのが堅牢）
     */
    if ((errcode = getaddrinfo(hostnm, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /*
     * 表示用に「数値表現のホスト/サービス」を得る
     * - NI_NUMERICHOST/NI_NUMERICSERV で DNS 名ではなく数値として表示
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

    /* ソケット生成 */
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }

    /* TCP コネクト */
    if (connect(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("connect");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    freeaddrinfo(res0);
    return (soc);
}

/* 前方宣言（send_recv_loop から呼ぶ） */
int recv_data(void);

/*
 * 送受信メインループ（I/O 多重化）
 *
 * ここがこのプログラムの“中核”:
 * - select() を使って、以下の 2 つを同時に待つ
 *   - 標準入力 fd=0（キーボード入力）
 *   - 接続ソケット g_soc（サーバからの受信）
 *
 * 端末を raw モードにしているので、
 * - getchar() は 1 文字単位ですぐ返る（行入力待ちではない）
 * - 入力した 1 文字をそのままサーバへ send() する
 */
void
send_recv_loop(void)
{
    struct timeval timeout;
    int width;
    fd_set mask, ready;
    char c;

    /*
     * 端末設定（学習用としては分かりやすいが、system() は副作用が大きい）
     * - -echo: 入力文字をローカルに表示しない
     * - raw: 端末ドライバによる行編集/特殊文字処理を抑え、生のキー入力を得る
     *
     * これにより telnet っぽく「キーを押した瞬間に送信」できる。
     */
    (void) system("stty -echo raw");

    /*
     * バッファリング OFF:
     * - stdin/stdout を無バッファにして、表示/入力の遅延を減らす
     * - telnet 的な対話では遅延が体感に影響するため
     */
    (void) setbuf(stdin, NULL);
    (void) setbuf(stdout, NULL);

    /* select() 用の監視集合 mask を作る */
    FD_ZERO(&mask);
    FD_SET(0, &mask);       /* 標準入力（キーボード） */
    FD_SET(g_soc, &mask);   /* ソケット受信 */

    /*
     * select の第1引数 width は「監視対象 fd の最大値 + 1」
     * - ここでは g_soc の方が 0 より大きいので g_soc + 1 で良い
     */
    width = g_soc + 1;

    for (;;) {
        /* select() は fd_set を破壊するので毎回 ready にコピーする */
        ready = mask;

        /* タイムアウト（ここでは 1 秒）: 無限待ちにしないため */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        /*
         * select(width, readfds, writefds, exceptfds, timeout)
         * - readfds に ready を渡し、「読めるようになった fd」を待つ
         */
        switch (select(width, &ready, NULL, NULL, &timeout)) {
        case -1:
            /*
             * シグナル割り込み(EINTR)はよくあるので無視して回す
             * それ以外はエラーとして終了フラグを立てる
             */
            if (errno != EINTR) {
                perror("select");
                g_end = 1;
            }
            break;

        case 0:
            /* タイムアウト: 定期的に g_end を見るだけ（何もしない） */
            break;

        default:
            /*
             * レディ有り:
             * - 受信できる（ソケット）
             * - 入力できる（標準入力）
             * のどちらか（または両方）
             */

            if (FD_ISSET(g_soc, &ready)) {
                /*
                 * サーバから受信可能:
                 * - recv_data() は telnet の IAC コマンドを判別して応答も返す
                 * - 受信失敗/切断なら終了
                 */
                if (recv_data() == -1) {
                    g_end = 1;
                    break;
                }
            }

            if (FD_ISSET(0, &ready)) {
                /*
                 * キーボード入力可能:
                 * - raw モードなので 1 文字すぐ取れる
                 * - そのまま 1 byte を send する
                 */
                c = getchar();
                if (send(g_soc, &c, 1, 0) == -1) {
                    perror("send");
                    g_end = 1;
                    break;
                }
            }
            break;
        }

        /* シグナル等で終了フラグが立ったら抜ける */
        if (g_end) {
            break;
        }
    }

    /*
     * 端末設定を元に戻す:
     * - echo: 表示を戻す
     * - cooked: 行編集ありの通常モードに戻す
     * - -istrip: 8bit を落とす設定を解除（環境によっては必要）
     */
    (void) system("stty echo cooked -istrip");
}

/*
 * サーバから 1 byte 受信して処理する
 *
 * Telnet は「データ」と「制御コマンド」が同じストリームに混ざる。
 * - 制御コマンドは IAC(255) から始まる
 *   IAC <command> <option>
 *
 * ここでは簡略化して:
 * - IAC を受け取ったら 2 バイト追加で読み、
 * - その option に対して常に WONT で否定応答する
 *
 * 注意:
 * - 本来は WILL/WONT/DO/DONT の意味や option ごとの挙動がある
 * - ここは「とりあえず全部拒否して表示を進める」最小実装
 */
int
recv_data(void)
{
    char buf[8];
    char c;

    /* 1 byte 受信（0 以下なら切断/エラー扱い） */
    if (recv(g_soc, &c, 1, 0) <= 0) {
        return (-1);
    }

    /*
     * IAC (Interpret As Command) 判定
     * - c は signed char の可能性があるので、& 0xFF して 0..255 に正規化して比較する
     */
    if ((int) (c & 0xFF) == IAC) {
        /*
         * Telnet コマンド:
         *   IAC <cmd> <opt>
         * を読む（最小実装なので 2 byte 追加で読む）
         */
        if (recv(g_soc, &c, 1, 0) == -1) {
            perror("recv");
            return (-1);
        }
        if (recv(g_soc, &c, 1, 0) == -1) {
            perror("recv");
            return (-1);
        }

        /*
         * option を “常に拒否” する:
         * - IAC WONT <opt> を返す
         * - 本来は cmd に応じて DO/DONT/WILL/WONT を選ぶべきだが、
         *   学習用として「交渉を全部拒否する」ことで単純化している
         */
        (void) snprintf(buf, sizeof(buf), "%c%c%c", IAC, WONT, c);
        if (send(g_soc, buf, 3, 0) == -1) {
            perror("send");
            return (-1);
        }
    } else {
        /* 通常データ: 画面へ 1 byte 出力 */
        (void) fputc(c & 0xFF, stdout);
    }

    return (0);
}

/*
 * シグナルハンドラ
 * - ここでは「終了したい」ことだけを main ループに伝える
 * - 重い処理（printf など）を避け、g_end だけ立てるのが安全
 */
void
sig_term_handler(int sig)
{
    g_end = sig; /* どのシグナルで終わったかを格納（0 以外なら終了） */
}

/*
 * シグナルの設定
 * - Ctrl+C (SIGINT) や kill (SIGTERM) などを捕まえて安全にループを抜ける
 * - raw モード中に異常終了すると端末が壊れた状態になりやすいので、捕まえる価値が高い
 */
void
init_signal(void)
{
    (void) signal(SIGINT,  sig_term_handler);
    (void) signal(SIGTERM, sig_term_handler);
    (void) signal(SIGQUIT, sig_term_handler);
    (void) signal(SIGHUP,  sig_term_handler);
}

int
main(int argc, char *argv[])
{
    char *port;

    /*
     * 引数:
     *   telnet1 hostname [port]
     * - port を省略したら "telnet"（通常 23/tcp）を使う
     */
    if (argc <= 1) {
        (void) fprintf(stderr, "telnet1 hostname [port]\n");
        return (EX_USAGE);
    } else if (argc <= 2) {
        port = "telnet";
    } else {
        port = argv[2];
    }

    /* サーバへ接続して g_soc に保持 */
    if ((g_soc = client_socket(argv[1], port)) == -1) {
        return (EX_IOERR);
    }

    /* シグナル設定（raw 端末復帰のためにも重要） */
    init_signal();

    /* メイン（I/O 多重化 + telnet コマンド最小処理） */
    send_recv_loop();

    /* 終了処理: ソケットを閉じる */
    if (g_soc != -1) {
        (void) close(g_soc);
    }

    (void) fprintf(stderr, "Connection Closed.\n");
    return (EX_OK);
}
