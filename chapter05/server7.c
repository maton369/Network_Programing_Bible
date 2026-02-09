/*
 * server7.c（マルチプロセス + lockf による accept 多重化制御）に
 * 「アルゴリズムの流れ」「ライブラリ/APIの使い方」が分かるように
 * 詳細コメントを追加した版。
 *
 * このプログラムの狙い（超重要）：
 * - 親プロセスが listen 済みのサーバソケット soc を作る
 * - fork() で NUM_CHILD 個の子プロセスを生成し、全員が同じ soc を共有して accept() を呼ぶ
 * - ただし「同時に複数プロセスが accept() へ突入」すると、教育用途として挙動が分かりづらい
 *   （/ OSによっては accept の “thundering herd” 問題に見える）
 * - そこで lockf() によるファイルロックを使い、
 *   「accept() に入るのは常に 1 プロセスだけ」に直列化する（多重化の制御）
 *
 * つまり：
 *   “複数プロセスでサーバを並列処理” しつつ、
 *   “accept() だけはロックで順番待ち” にしている。
 *
 * なお実運用では：
 * - Linux では accept の thundering herd はカーネル側で改善されている/されていないなど歴史があり、
 *   また設計としては SO_REUSEPORT + 複数 listen、または epoll などのイベント駆動が一般的。
 * - ただ教材としては「プロセス間排他（ファイルロック）」「fork 後の FD 共有」
 *   「accept とワーカ処理の責務分離」を一度に学べて良い例である。
 */

#include <sys/file.h>                   /* lockf() / flock() 系のため（実装により） */
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

/* ※このコードでは open() を使っているので本来 <fcntl.h> が必要（O_RDWR/O_CREAT） */
#include <fcntl.h>                      /* ★追加：open(), O_RDWR, O_CREAT の定義 */

/* 生成する子プロセス数（同時ワーカー数の上限に近いイメージ） */
#define NUM_CHILD 2

/*
 * ロック用のファイル名。
 * - open() で FD を得て、その FD に対して lockf() で排他する。
 * - unlink() でパス名は消すが FD は生き続けるので、ロック自体は有効。
 *   （テンポラリファイルの “名前だけ消す” テクニック）
 */
#define LOCK_FILE "./server7.lock"

/* ロック用ファイルディスクリプタ（全プロセスで共有される “同じオープンファイル”） */
int g_lock_fd = -1;

/* プロトタイプ宣言（このコードは関数が後ろにあるので宣言が必要） */
void accept_loop(int soc);
void send_recv_loop(int acc);
size_t mystrlcat(char *dst, const char *src, size_t size);

/* サーバソケットの準備（listen まで） */
int
server_socket(const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    /*
     * getaddrinfo のヒント：
     * - ai_family = AF_INET : IPv4
     * - ai_socktype = SOCK_STREAM : TCP
     * - ai_flags = AI_PASSIVE : bind 用（NULL を渡すと 0.0.0.0 相当）
     */
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    /* どのアドレスに bind するか決める（ここでは NULL なので任意アドレス） */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* 実際に決まった bind 先（数値表示）をログ出しする（学習用） */
    if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen,
                               nbuf, sizeof(nbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
        freeaddrinfo(res0);
        return (-1);
    }
    (void) fprintf(stderr, "port=%s\n", sbuf);

    /* TCPソケット作成 */
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }

    /*
     * SO_REUSEADDR：
     * - サーバ再起動直後（TIME_WAIT が残っている）でも bind できるようにする定番設定。
     */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* ソケットにアドレスを割り当て（待受ポートを確保） */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* listen：待受状態へ。SOMAXCONN はバックログの上限（OS依存） */
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
 * accept_loop（子プロセス側のメインループ）
 *
 * アルゴリズムの要点：
 * - 複数の子プロセスが同じ soc を共有している（fork 後も FD はコピーされる）
 * - そのまま全員が accept() をすると “同時に accept に突っ込む” ことになる
 * - ここでは lockf を使い、「accept に入る前に排他ロック」を取る
 *   → accept を呼べるのは常に 1 プロセスのみ（直列化）
 *
 * lockf の使い方：
 * - lockf(fd, F_LOCK, 0)   : ブロッキングでロック獲得（取れるまで待つ）
 * - lockf(fd, F_ULOCK, 0)  : ロック解放
 * - lockf(fd, F_TEST, 0)   : ロック可能なら 0、不可なら -1（errno=EACCES/EAGAIN 等）
 *
 * ここでの “ロックの役割”：
 * - OSの accept 実装・wake-up の挙動に依存しない形で、
 *   「どの子が accept を処理したか」を明確に観察できるようにする。
 */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;
    int acc;
    socklen_t len;

    for (;;) {
        /* ====== accept に入る前に排他制御 ====== */
        (void) fprintf(stderr, "<%d>ロック獲得開始\n", getpid());

        /*
         * F_LOCK はブロッキング：他プロセスがロック中ならここで待つ。
         * つまり “accept 待ち行列” に入る前に “ロック待ち行列” を作っている。
         */
        (void) lockf(g_lock_fd, F_LOCK, 0);

        (void) fprintf(stderr, "<%d>ロック獲得！\n", getpid());

        /* accept で接続を受ける（この瞬間にロックを握っているのはこのプロセスだけ） */
        len = (socklen_t) sizeof(from);
        if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
            /*
             * accept が失敗するケース：
             * - EINTR : シグナル割り込み（ここでは継続したい）
             * - それ以外：エラー
             * 失敗した場合でもロックは必ず解放する必要がある。
             */
            if (errno != EINTR) {
                perror("accept");
            }
            (void) fprintf(stderr, "<%d>ロック解放\n", getpid());
            (void) lockf(g_lock_fd, F_ULOCK, 0);
        } else {
            /* 接続元のIP/portをログに出す（観察用） */
            (void) getnameinfo((struct sockaddr *) &from, len,
                               hbuf, sizeof(hbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV);

            (void) fprintf(stderr, "<%d>accept:%s:%s\n", getpid(), hbuf, sbuf);

            /*
             * accept が完了したら “すぐロックを解放” するのがポイント：
             * - このプロセスは今から send/recv でクライアント処理を行う
             * - ロックを握ったままだと、他の子が accept できず並列性が消える
             * - つまりロックは「accept の直列化」だけに使い、
             *   その後の I/O 処理は各プロセスが並列に実行できるようにする。
             */
            (void) fprintf(stderr, "<%d>ロック解放\n", getpid());
            (void) lockf(g_lock_fd, F_ULOCK, 0);

            /* 接続（acc）に対して送受信処理（この間、別の子が accept 可能） */
            send_recv_loop(acc);

            /* 1接続の処理が終わったら acc をクローズ（プロセスが責務を持つ） */
            (void) close(acc);
        }
    }
}

/* サイズ指定文字列連結（strlcat 相当：バッファサイズを超えないように連結） */
size_t
mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    /* dst の終端まで進める（size を超えない範囲で） */
    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--)
        ;
    dlen = (size_t)(pd - dst);

    /* dst が既に size いっぱいなら連結できない（必要長だけ返す） */
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }

    /* dst の末尾（終端 NUL を置ける位置まで） */
    pde = dst + size - 1;

    /* src を dst にコピー（終端を超えない） */
    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    /* NUL 終端する（残りを 0 で埋める） */
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }

    /* src の長さを数える（戻り値計算用） */
    while (*ps++)
        ;
    return (dlen + (size_t)(ps - src - 1));
}

/*
 * send_recv_loop（子プロセスが “担当した接続” を処理する）
 *
 * 重要：
 * - ここは accept のロックとは無関係に動く（並列に動作可能）
 * - 各子プロセスは自分が accept した acc だけを触る
 */
void
send_recv_loop(int acc)
{
    char buf[512], *ptr;
    ssize_t len;

    for (;;) {
        /* 受信（TCPストリームなので “1回の recv が1行” とは限らないが教材では簡略化） */
        if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
            perror("recv");
            break;
        }
        if (len == 0) {
            /* 相手が close した（EOF） */
            (void) fprintf(stderr, "<%d>recv:EOF\n", getpid());
            break;
        }

        /* 受信データを文字列として扱うために終端（※len==512だと範囲外なので本来防御が必要） */
        buf[len] = '\0';

        /* 改行を切ってログ表示しやすくする */
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }
        (void) fprintf(stderr, "<%d>[client]%s\n", getpid(), buf);

        /* 応答を作って返す（元文字列 + ":OK\r\n"） */
        (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
        len = (ssize_t) strlen(buf);

        if (send(acc, buf, (size_t)len, 0) == -1) {
            perror("send");
            break;
        }
    }
}

int
main(int argc, char *argv[])
{
    int i, soc;
    pid_t pid;

    /* 引数チェック */
    if (argc <= 1) {
        (void) fprintf(stderr, "server7 port\n");
        return (EX_USAGE);
    }

    /* listen 用ソケットを準備（親が1回だけ作る → fork 後は子と共有） */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    /*
     * ロックファイルを open して FD を得る
     * - この FD に対して lockf で排他制御を行う
     * - fork 後、子プロセスも同じ FD（同じ open file description）を引き継ぐので
     *   プロセス間排他として機能する
     */
    if ((g_lock_fd = open(LOCK_FILE, O_RDWR | O_CREAT, 0666)) == -1) {
        perror("open");
        return (EX_UNAVAILABLE);
    }

    /*
     * ファイル名を unlink して “名前だけ消す”
     * - 実体（inode）は FD が閉じられるまで残る
     * - プログラム終了時にファイルが残らず後始末が楽
     */
    (void) unlink(LOCK_FILE);

    (void) fprintf(stderr, "start %d children\n", NUM_CHILD);

    /* 子プロセスを NUM_CHILD 個生成 */
    for (i = 0; i < NUM_CHILD; i++) {
        if ((pid = fork()) == 0) {
            /*
             * 子プロセス：
             * - soc と g_lock_fd を共有している
             * - accept_loop に入り、ロックで accept を直列化しつつ
             *   自分が取った接続を処理する
             */
            accept_loop(soc);
            _exit(1);
        } else if (pid > 0) {
            /* 親プロセス：生成を続けるだけ（この例では accept しない） */
        } else {
            /* fork失敗 */
            perror("fork");
        }
    }

    (void) fprintf(stderr, "ready for accept\n");

    /*
     * 親プロセスは “デモ用の監視” をする：
     * - lockf(F_TEST) でロック状態を観察する（学習用）
     *   返り値 0 ならロック可能（誰も握ってない）
     *   -1 ならロック不可（誰かが握ってる）※errnoで詳細
     *
     * 注意：
     * - この親は accept をしないので、実際の接続処理は子だけが行う。
     */
    for (;;) {
        (void) sleep(10);
        (void) fprintf(stderr,
                       "<<%d>>ロック状態：%d\n",
                       getpid(),
                       lockf(g_lock_fd, F_TEST, 0));
    }

    /* NOT REACHED（ここには来ない） */
    (void) close(soc);
    (void) close(g_lock_fd);
    return (EX_OK);
}

/*
 * 追加で気づき（改善ポイント：教材外だが重要）
 *
 * 1) <fcntl.h> が無いと open/O_CREAT が未定義になり得る（コンパイル警告/エラー）
 *    → このコメント付き版では <fcntl.h> を追加している。
 *
 * 2) buf[len] = '\0' が境界外になる可能性：
 *    recv のサイズが sizeof(buf) のとき len==512 になり得る。
 *    → 文字列化するなら recv を sizeof(buf)-1 にするのが安全。
 *
 * 3) lockf のエラー処理：
 *    このコードは lockf の返り値をチェックしていない。
 *    教材としては挙動観察が主だが、実務なら戻り値と errno を見るべき。
 *
 * 4) マルチプロセスで accept を直列化すると、accept の並列性は消える：
 *    ただし “accept は短時間” なので、I/O処理の並列性（send_recv_loop）が主役になる。
 *    ロックはあくまで教育目的（誰が accept したかを明確にする）である。
 */
