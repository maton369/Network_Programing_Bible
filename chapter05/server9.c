/*
    server9.c（epoll + 送信専用スレッド群 + キュー）に詳細コメントを付与した版

    このコードの狙い（全体像）
    --------------------------
    - accept/recv など「受信側のイベント検知」は epoll で 1 本のスレッド（メイン）に集約する
    - send（応答送信）とログ出力など「重くなりがちな処理」は送信専用スレッドに渡す
    - メインスレッドは「受信してキューに積む」までで戻るので、epoll ループが詰まりにくい

    重要なアイデア
    --------------
    - epoll で "受信できる状態になったソケット" をまとめて拾う（多重化）
    - recv した結果（acc, buf, len）をリングバッファ（queue）へ push する（producer）
    - 送信スレッドがリングバッファから pop して send する（consumer）
    - producer/consumer を mutex + cond で同期する

    注意（学習用として理解しておくポイント）
    --------------------------------------
    - この実装は「受信は epoll スレッド、送信は別スレッド」という分離であり、
      「1接続=1スレッド」型ではなく、イベント駆動 + ワーカー（送信）という構造に近い。
    - キューのオーバーフロー対策が薄い（MAXQUEUESZ を超えると last が front を追い越し得る）。
      本番では「満杯なら捨てる / ブロック / 拡張」など方針が必要。
    - epoll 側で recv したあとに mutex を取って last を進めているが、
      recv 実行自体は mutex 外で行われている。ここは設計として “どこを排他すべきか” を意識する。
*/

#include <sys/epoll.h>                  /* epoll_create / epoll_ctl / epoll_wait */
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>                    /* pthread_* */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/* リングバッファ（キュー）の最大要素数
   - 4096 件まで「受信済みデータ（acc, buf, len）」を溜められる想定 */
#define MAXQUEUESZ 4096

/* 送信スレッド（= キュー）の数
   - MAXSENDER 個のキューを用意し、fd % MAXSENDER で振り分ける */
#define MAXSENDER  2

/* リングバッファの次のインデックス（循環） */
#define QUEUE_NEXT(i_)             (((i_) + 1) % MAXQUEUESZ)

/* キューに積む 1 件分のデータ
   - acc: 接続ソケット FD
   - buf: 受信バッファ（メッセージ）
   - len: recv で得たバイト数（-1: error, 0: EOF, >0: 正常） */
struct queue_data {
    int acc;
    char buf[512];
    ssize_t len;
};

/* producer-consumer 用リングバッファ
   - front: 次に取り出す位置（consumer が使う）
   - last : 次に書き込む位置（producer が使う）
   - mutex: front/last/data へのアクセス保護
   - cond : 「新規データが来た」ことを consumer に通知するため */
struct queue {
    int front;
    int last;
    struct queue_data data[MAXQUEUESZ];
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

/* 送信スレッド数分のキュー（qi=0..MAXSENDER-1） */
struct queue g_queue[MAXSENDER];

/* サーバソケットの準備 */
int
server_socket(const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    /* getaddrinfo のヒントを初期化 */
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;    /* TCP */
    hints.ai_flags = AI_PASSIVE;        /* サーバ用途（NULL host で全IFに bind） */

    /* バインド先アドレス（portnm）を解決 */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* 表示用に数値化して出す（学習用ログ） */
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

    /* SO_REUSEADDR：再起動時の bind 失敗を減らす（TIME_WAIT 対策） */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* bind：ローカルアドレス（ポート）をソケットに割り当て */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* listen：受動オープン開始。SOMAXCONN は OS 依存の最大バックログ */
    if (listen(soc, SOMAXCONN) == -1) {
        perror("listen");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    freeaddrinfo(res0);
    return (soc);
}

/* 同時に epoll 管理する最大接続数（学習用） */
#define    MAX_CHILD    (20)

/* アクセプトループ（epoll で accept と recv を多重化）
   - listening socket (soc) + 接続ソケット（acc群）を epoll に登録
   - epoll_wait で「読み取り可能」になった FD を拾う
   - listening socket が ready → accept して新しい接続を epoll に追加
   - 接続ソケットが ready → recv してキューへ push、送信スレッドに処理を渡す */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;

    int acc;            /* accept で返る接続ソケット */
    int count;          /* 現在管理中の接続数（MAX_CHILD 以内に制限） */
    int i;              /* ループ用 */
    int qi;             /* キュー番号（fd % MAXSENDER） */
    int epollfd;        /* epoll インスタンス FD */
    int nfds;           /* epoll_wait で返るイベント件数 */

    socklen_t flen;     /* accept/getnameinfo 用 */

    struct epoll_event ev;
    struct epoll_event events[MAX_CHILD + 1]; /* soc + acc で最大 MAX_CHILD+1 */

    /* epoll インスタンス生成
       - Linux では size 引数は無視されるが、1 を渡すのが一般的 */
    if ((epollfd = epoll_create(1)) == -1) {
        perror("epoll_create");
        return;
    }

    /* listening socket を epoll へ登録（読み取り可能=接続到来） */
    ev.data.fd = soc;
    ev.events = EPOLLIN;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, soc, &ev) == -1) {
        perror("epoll_ctl");
        (void) close(epollfd);
        return;
    }

    count = 0;

    for (;;) {
        (void) fprintf(stderr,"<<child count:%d>>\n", count);

        /* epoll_wait：
           - events に ready FD を詰めて返す
           - timeout = 10秒（10*1000ms） */
        nfds = epoll_wait(epollfd, events, MAX_CHILD + 1, 10 * 1000);

        switch (nfds) {
        case -1:
            /* エラー（EINTR で割り込みの可能性もあるが、ここでは単純化） */
            perror("epoll_wait");
            break;

        case 0:
            /* タイムアウト：何も起きなかった */
            break;

        default:
            /* ready FD が nfds 件 */
            for (i = 0; i < nfds; i++) {

                /* ready FD が listening socket なら accept */
                if (events[i].data.fd == soc) {

                    flen = (socklen_t) sizeof(from);

                    /* 接続受付（新しい acc を得る） */
                    acc = accept(soc, (struct sockaddr *) &from, &flen);
                    if (acc == -1) {
                        if (errno != EINTR) {
                            perror("accept");
                        }
                        continue;
                    }

                    /* 相手をログ表示 */
                    (void) getnameinfo((struct sockaddr *) &from, flen,
                                       hbuf, sizeof(hbuf),
                                       sbuf, sizeof(sbuf),
                                       NI_NUMERICHOST | NI_NUMERICSERV);
                    (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

                    /* 接続数制限（学習用） */
                    if (count + 1 >= MAX_CHILD) {
                        (void) fprintf(stderr, "connection is full : cannot accept\n");
                        (void) close(acc);
                        continue;
                    }

                    /* acc を epoll に追加（受信可能を監視） */
                    ev.data.fd = acc;
                    ev.events = EPOLLIN;
                    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, acc, &ev) == -1) {
                        perror("epoll_ctl");
                        (void) close(acc);
                        (void) close(epollfd);
                        return;
                    }
                    count++;
                    continue;
                }

                /* ここに来るのは「接続ソケットが ready」なケース（=受信できる） */
                {
                    int fd = events[i].data.fd;

                    /* fd を送信スレッド（キュー）へ割り当て
                       - 単純に fd % MAXSENDER で振り分け（負荷分散の簡易版） */
                    qi = fd % MAXSENDER;

                    /* ここでは g_queue[qi].last のスロットへ受信結果を格納する。
                       注意：この時点では mutex を取っていないので、
                             producer が複数いる設計にすると破綻する。
                             今回は accept_loop が 1 スレッド（producer 1本）なので成立している。 */
                    g_queue[qi].data[g_queue[qi].last].acc = fd;

                    g_queue[qi].data[g_queue[qi].last].len =
                        recv(g_queue[qi].data[g_queue[qi].last].acc,
                             g_queue[qi].data[g_queue[qi].last].buf,
                             sizeof(g_queue[qi].data[g_queue[qi].last].buf),
                             0);

                    /* recv の結果で分岐 */
                    switch (g_queue[qi].data[g_queue[qi].last].len) {

                    case -1:
                        /* エラー（EAGAIN 等の可能性もあるが、この実装は単純化） */
                        perror("recv");
                        /* fall through */

                    case 0:
                        /* EOF：クライアント切断 */
                        (void) fprintf(stderr, "[child%d]recv:EOF\n", fd);

                        /* epoll から削除（監視不要に） */
                        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev) == -1) {
                            perror("epoll_ctl");
                            (void) close(epollfd);
                            return;
                        }

                        /* ソケットクローズ */
                        (void) close(fd);
                        count--;
                        break;

                    default:
                        /* 正常受信：キューへ “1件追加” を確定させる
                           - last を進める操作は共有データなので mutex で保護
                           - cond_signal で送信スレッドを起こす */
                        (void) pthread_mutex_lock(&g_queue[qi].mutex);

                        g_queue[qi].last = QUEUE_NEXT(g_queue[qi].last);

                        (void) pthread_cond_signal(&g_queue[qi].cond);
                        (void) pthread_mutex_unlock(&g_queue[qi].mutex);
                        break;
                    }
                }
            }
            break;
        }
    }

    /* 通常は到達しないが、終了パスとして close */
    (void) close(epollfd);
}

/* サイズ指定文字列連結（strlcat 風）
   - dst のサイズ上限 size を超えないように src を連結する
   - 返り値は “連結後に必要だった長さ” 相当（strlcat の慣例） */
size_t
mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    /* dst の終端（あるいは size 到達）まで進める */
    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--)
        ;

    dlen = (size_t)(pd - dst);

    /* dst がすでに size いっぱいなら、src を全部足した長さを返すだけ */
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }

    /* pde は “書き込み可能な最後の位置” (NUL 終端用に -1) */
    pde = dst + size - 1;

    /* src を dst にコピー（pde を超えない範囲） */
    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    /* 残りを NUL で埋める（安全側） */
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }

    /* src の残り長を数える */
    while (*ps++)
        ;

    return (dlen + (size_t)(ps - src - 1));
}

/* 送信スレッド（consumer）
   - qi（0..MAXSENDER-1）に対応するキューからデータを取り出して応答する
   - キューが空なら cond_wait でスリープし、producer からの signal で起きる */
void *
send_thread(void *arg)
{
    char *ptr;
    ssize_t len;

    int i;   /* pop した要素のインデックス */
    int qi;  /* 自分のキュー番号 */

    /* 引数：qi を受け取る
       注意：本来は intptr_t を使うのが安全（64bit 環境でのポインタ/整数変換） */
    qi = (int) arg;

    for (;;) {
        /* キューの排他制御開始 */
        (void) pthread_mutex_lock(&g_queue[qi].mutex);

        if (g_queue[qi].last != g_queue[qi].front) {
            /* キューにデータがある：front を 1つ進めて pop */
            i = g_queue[qi].front;
            g_queue[qi].front = QUEUE_NEXT(g_queue[qi].front);

            /* pop が確定したのでロック解除（以降は自分だけが i の要素を触る想定） */
            (void) pthread_mutex_unlock(&g_queue[qi].mutex);
        } else {
            /* キューが空：新しいデータが来るまで待つ
               - cond_wait は mutex を一時解放し、起床時に再ロックして戻る */
            (void) pthread_cond_wait(&g_queue[qi].cond, &g_queue[qi].mutex);
            (void) pthread_mutex_unlock(&g_queue[qi].mutex);
            continue;
        }

        /* ここからは “i の要素” を処理して応答する */

        /* 受信バッファを NUL 終端して文字列として扱えるようにする */
        g_queue[qi].data[i].buf[g_queue[qi].data[i].len] = '\0';

        /* CR/LF を潰してログを整形 */
        if ((ptr = strpbrk(g_queue[qi].data[i].buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }

        /* ログ出力（child は fd を出しているが、ここでは acc を表示） */
        (void) fprintf(stderr,
                       "[child%d]%s\n",
                       g_queue[qi].data[i].acc,
                       g_queue[qi].data[i].buf);

        /* 応答文字列を作成（末尾に :OK\r\n） */
        (void) mystrlcat(g_queue[qi].data[i].buf,
                         ":OK\r\n",
                         sizeof(g_queue[qi].data[i].buf));

        g_queue[qi].data[i].len = (ssize_t) strlen(g_queue[qi].data[i].buf);

        /* 応答送信 */
        if ((len = send(g_queue[qi].data[i].acc,
                        g_queue[qi].data[i].buf,
                        (size_t) g_queue[qi].data[i].len, 0)) == -1) {
            perror("send");
        }

        /* この実装では send 失敗時も切断処理まではしない（学習用簡略） */
    }

    pthread_exit((void *) 0);
    return ((void *) 0);
}

int
main(int argc, char *argv[])
{
    int soc, i;
    pthread_t id;

    /* 引数：ポート番号 */
    if (argc <= 1) {
        (void) fprintf(stderr,"server9 port\n");
        return (EX_USAGE);
    }

    /* 送信スレッド（consumer）を MAXSENDER 本起動し、対応するキューを初期化 */
    for (i = 0; i < MAXSENDER; i++) {
        /* mutex / cond の初期化（queue.front/last はゼロ初期値を前提） */
        (void) pthread_mutex_init(&g_queue[i].mutex, NULL);
        (void) pthread_cond_init(&g_queue[i].cond, NULL);

        /* 送信スレッド生成
           - i を arg に渡す（qi）
           注意：64bit 環境では (void*)i が危険なので、本来は (void*)(intptr_t)i が良い */
        (void) pthread_create(&id, NULL, (void *) send_thread, (void *) i);
    }

    /* listening socket を作成 */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr,"server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    (void) fprintf(stderr, "ready for accept\n");

    /* accept + recv + enqueue（producer）はメインスレッドで担当 */
    accept_loop(soc);

    /* accept_loop は基本戻らないが、形式的に join（最後に作った id しか join してない点に注意） */
    pthread_join(id, NULL);

    (void) close(soc);
    return (EX_OK);
}
