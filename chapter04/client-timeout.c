/*
 * connect() に “タイムアウト” を付ける TCP クライアント（client-timeout）
 *
 * 目的：
 * - 通常の connect() は（環境によって）長時間ブロックする可能性がある
 * - そこで、ソケットを一時的にノンブロッキングにして connect を開始し、
 *   select() で「一定時間以内に接続完了したか」を監視することでタイムアウトを実現する
 *
 * このコードの全体アルゴリズムは大きく 2 部分：
 *
 * [A] 接続確立（タイムアウト可能 connect）
 *   1) getaddrinfo() で接続先アドレスを得る
 *   2) socket() でソケット生成
 *   3) timeout_sec < 0 のときは通常の blocking connect()
 *   4) timeout_sec >= 0 のときは：
 *        - ソケットを O_NONBLOCK にする
 *        - connect() を呼ぶ（即時成功 or EINPROGRESS）
 *        - select() で “書き込み可能” になるのを timeout 付きで待つ
 *        - getsockopt(SO_ERROR) で接続成功/失敗を確定する
 *        - 成功したら元の blocking に戻す
 *
 * [B] 接続後の送受信（対話型）
 *   - select() で「ソケット受信」と「標準入力」を多重化する
 *   - 受信できたら表示、入力できたら送信
 *
 * 学習ポイント：
 * - “connect のタイムアウト” は connect 自体に直接 timeout を指定できないことが多い
 * - ノンブロッキング + select + SO_ERROR という定番手法で実現する
 * - select で ready になっただけでは「成功とは限らない」ので SO_ERROR で確定する
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>

/* fcntl と O_NONBLOCK を使うために追加 */
#include <fcntl.h>  /* Chapter 01「TCP/IPプログラミング入門」の client.c に追加 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/* ブロッキング/ノンブロッキングモード切替
 *
 * fd   : 対象ファイルディスクリプタ（ここではソケット）
 * flag : 0 → ノンブロッキング, 1 → ブロッキング
 *
 * アルゴリズム：
 * 1) fcntl(fd, F_GETFL) で現在のフラグを取得
 * 2) O_NONBLOCK を ON/OFF したフラグを fcntl(fd, F_SETFL) で反映
 *
 * 学習ポイント：
 * - connect タイムアウトを実装するために “一時的にノンブロッキングにする” のが目的
 * - 成功後はブロッキングに戻して、以降の send/recv の挙動を単純にしている
 */
int
set_block(int fd, int flag)
{
    int flags;

    /* 現在のファイル状態フラグを取得 */
    if ((flags = fcntl(fd, F_GETFL, 0)) == -1) {
        perror("fcntl");
        return (-1);
    }

    if (flag == 0) {
        /* ノンブロッキング：O_NONBLOCK を追加 */
        (void) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } else if (flag == 1) {
        /* ブロッキング：O_NONBLOCK を除去 */
        (void) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    return (0);
}

/* サーバにソケット接続（タイムアウト可能版）
 *
 * hostnm      : 接続先ホスト名（例: "127.0.0.1", "example.com"）
 * portnm      : 接続先ポート（文字列）
 * timeout_sec : -1 ならタイムアウト無し（通常の connect）
 *               0以上ならその秒数で connect の完了を待つ
 *
 * 戻り値：
 * - 成功：接続済みソケット FD
 * - 失敗：-1
 *
 * タイムアウト付き connect の定番アルゴリズム：
 * 1) socket を O_NONBLOCK にして connect()
 * 2) connect が -1 で errno==EINPROGRESS なら「接続処理が進行中」
 * 3) select で “書き込み可能” になるのを timeout 付きで待つ
 * 4) ready になったら getsockopt(SO_ERROR) で結果を確定
 *    - val==0 なら connect 成功
 *    - val!=0 なら connect 失敗（val がエラー番号）
 * 5) 成功時は O_NONBLOCK を外してブロッキングに戻す
 */
int
client_socket_with_timeout(const char *hostnm,
                           const char *portnm,
                           int timeout_sec)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    struct timeval timeout;
    int soc, errcode, width, val;
    socklen_t len;
    fd_set mask, write_mask, read_mask;

    /* getaddrinfo のヒントをゼロクリアして必要項目だけ設定 */
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */

    /* アドレス情報の決定（host:port の sockaddr を得る） */
    if ((errcode = getaddrinfo(hostnm, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* デバッグ表示：実際に使う数値アドレス/ポート */
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

    if (timeout_sec < 0) {
        /* タイムアウト無し（通常の blocking connect） */
        if (connect(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
            perror("connect");
            (void) close(soc);
            freeaddrinfo(res0);
            return (-1);
        }
        freeaddrinfo(res0);
        return (soc);
    } else {
        /* タイムアウト有り */

        /* 1) ノンブロッキングモードに切り替える */
        (void) set_block(soc, 0);

        /* 2) connect 開始
         * - すぐ成功する場合もある（ローカルや既知経路など）
         * - 進行中の場合は -1 で errno==EINPROGRESS になる
         */
        if (connect(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
            if (errno != EINPROGRESS) {
                /* 進行中以外：即エラー */
                perror("connect");
                (void) close(soc);
                freeaddrinfo(res0);
                return (-1);
            }
            /* errno==EINPROGRESS：接続処理が進行中なので select で待つ */
        } else {
            /* connect が即時に成功した */
            (void) set_block(soc, 1);   /* 以降の I/O を単純化するためブロッキングへ戻す */
            freeaddrinfo(res0);
            return (soc);
            /*NOT REACHED*/
        }

        /* 3) connect 結果待ち：select で “接続完了相当” を待つ
         *
         * なぜ write_mask を見るのか：
         * - TCP の connect 完了時、ソケットは「書き込み可能」になることが多い
         * - ただし例外もあるため read_mask も一応見ている（エラー通知等）
         *
         * 注意：
         * - select が返ってきただけでは「成功」とは限らない
         * - 必ず getsockopt(SO_ERROR) で最終結果を確定させるのが定石
         */
        FD_ZERO(&mask);
        FD_SET(soc, &mask);
        width = soc + 1;

        timeout.tv_sec = timeout_sec;
        timeout.tv_usec = 0;

        for (;;) {
            /* select は fd_set を破壊するので毎回コピーして渡す */
            write_mask = mask;
            read_mask  = mask;

            switch (select(width, &read_mask, &write_mask, NULL, &timeout)) {
            case -1:
                /* select 自体のエラー（EINTR はシグナル割り込みなのでリトライ可能） */
                if (errno != EINTR) {
                    perror("select");
                    (void) close(soc);
                    freeaddrinfo(res0);
                    return (-1);
                }
                /* EINTR の場合はループ継続 */
                break;

            case 0:
                /* タイムアウト（指定秒数内に connect が確定しなかった） */
                (void) fprintf(stderr, "select:timeout\n");
                (void) close(soc);
                freeaddrinfo(res0);
                return (-1);

            default:
                /* ready になった（接続が成功したか失敗したかが確定した可能性が高い） */
                if (FD_ISSET(soc, &write_mask) || FD_ISSET(soc, &read_mask)) {

                    /* 4) getsockopt(SO_ERROR) で connect の結果を確定する
                     *
                     * SO_ERROR の意味：
                     * - val == 0 なら connect 成功
                     * - val != 0 なら connect 失敗（val が errno 相当）
                     *
                     * 注意：len は “val のサイズ” を渡すのが正しい
                     * - このコードでは len = sizeof(len) になっているが、
                     *   意図としては len = sizeof(val) が自然である
                     *   （環境によっては同サイズなので動くが、読みやすさ/正しさのため val を使う方が良い）
                     */
                    len = sizeof(len);  /* ← 教材としては len = sizeof(val) の方が意図が明確 */
                    if (getsockopt(soc, SOL_SOCKET, SO_ERROR, &val, &len) != -1) {
                        if (val == 0) {
                            /* connect 成功 */
                            (void) set_block(soc, 1);  /* 元に戻す */
                            freeaddrinfo(res0);
                            return (soc);
                        } else {
                            /* connect 失敗（val がエラー番号） */
                            (void) fprintf(stderr,
                                           "getsockopt:%d:%s\n",
                                           val,
                                           strerror(val));
                            (void) close(soc);
                            freeaddrinfo(res0);
                            return (-1);
                        }
                    } else {
                        /* getsockopt 自体が失敗 */
                        perror("getsockopt");
                        (void) close(soc);
                        freeaddrinfo(res0);
                        return (-1);
                    }
                }
                break;
            }
        }
    }
}

/* 送受信処理（対話型：標準入力とソケットを select で多重化）
 *
 * soc: 接続済みソケット FD
 *
 * アルゴリズム：
 * - select で「ソケット受信可能」と「標準入力読み込み可能」を監視
 * - ソケットが ready → recv して表示
 * - 標準入力が ready → 1行読み → send
 *
 * 学習ポイント：
 * - “1スレッドで I/O を多重化する” ための select の基本例
 * - クライアント側でもイベント駆動的に I/O を扱える
 *
 * 注意（境界）：
 * - buf[len]='\0' は len==512 のとき境界外アクセスになり得る
 *   → 安全化するなら recv(..., sizeof(buf)-1, ...) が定石
 */
void
send_recv_loop(int soc)
{
    char buf[512];
    struct timeval timeout;
    int end, width;
    ssize_t len;
    fd_set  mask, ready;

    /* select() 用マスクを初期化 */
    FD_ZERO(&mask);

    /* ソケットを監視対象に追加 */
    FD_SET(soc, &mask);

    /* 標準入力(0)も監視対象に追加 */
    FD_SET(0, &mask);

    width = soc + 1;  /* select の第1引数は “最大FD+1” */

    /* 送受信ループ */
    for (end = 0;; ) {
        /* select は fd_set を破壊するので毎回コピー */
        ready = mask;

        /* 1秒タイムアウト（教材的に “定期的に戻る” ようにしている） */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        switch (select(width, (fd_set *) &ready, NULL, NULL, &timeout)) {
        case -1:
            /* select エラー */
            perror("select");
            break;

        case 0:
            /* タイムアウト（何も起きなかった） */
            break;

        default:
            /* レディ有り */

            /* ソケットがレディ（受信可能） */
            if (FD_ISSET(soc, &ready)) {
                if ((len = recv(soc, buf, sizeof(buf), 0)) == -1) {
                    perror("recv");
                    end = 1;
                    break;
                }
                if (len == 0) {
                    /* 相手が切断 */
                    (void) fprintf(stderr, "recv:EOF\n");
                    end = 1;
                    break;
                }

                /* 受信データを表示（簡易に文字列化）
                 * NOTE: len==512 のとき buf[512] で境界外になり得る
                 */
                buf[len] = '\0';
                (void) printf("> %s", buf);
            }

            /* 標準入力がレディ（入力可能） */
            if (FD_ISSET(0, &ready)) {
                (void) fgets(buf, sizeof(buf), stdin);
                if (feof(stdin)) {
                    /* Ctrl+D 等で入力終了 */
                    end = 1;
                    break;
                }

                /* 送信（部分送信は未考慮：教材として単純化） */
                if ((len = send(soc, buf, strlen(buf), 0)) == -1) {
                    perror("send");
                    end = 1;
                    break;
                }
            }
            break;
        }

        if (end) {
            break;
        }
    }
}

int
main(int argc, char *argv[])
{
    int soc;

    /* 引数チェック：
     * argv[1] : server-host
     * argv[2] : port
     * argv[3] : timeout-sec（-1 ならタイムアウト無し）
     */
    if (argc <= 3) {
        (void) fprintf(stderr,
                       "client-timeout "
                       "server-host port timeout-sec(-1:no-timeout)\n");
        return (EX_USAGE);
    }

    /* サーバに接続（タイムアウト指定） */
    if ((soc = client_socket_with_timeout(argv[1], argv[2], atoi(argv[3])))
        == -1) {
        (void) fprintf(stderr, "client_socket_with_timeout():error\n");
        return (EX_UNAVAILABLE);
    }

    /* 接続後の対話型送受信 */
    send_recv_loop(soc);

    /* ソケットを閉じて終了 */
    (void) close(soc);
    return (EX_OK);
}

/*
 * 改善ポイント（次のステップ向け）
 *
 * 1) getsockopt の len
 * - len = sizeof(val) の方が意図が明確（現状は len = sizeof(len)）
 *
 * 2) select の timeout を EINTR で継続する場合の扱い
 * - 現状は timeout を再設定せずそのまま継続するため、意図とズレることがある
 * - “残り時間” を再計算する実装にするとより厳密
 *
 * 3) recv/send の境界・部分送信
 * - TCPはストリームなので recv で1行が必ず取れるとは限らない
 * - send も部分送信の可能性がある
 *
 * 4) IPv6対応
 * - hints.ai_family = AF_UNSPEC にして IPv4/IPv6 両対応にするのが一般的
 */
