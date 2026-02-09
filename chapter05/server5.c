/*
 * server5: fork() による “プロセス並列” TCP サーバ + SIGCHLD による子回収
 *
 * 目的：
 * - server2/3/4 の「1プロセスで多重化(select/poll/epoll)」とは別ルートとして、
 *   「接続ごとに fork() で子プロセスを作って処理する」古典的な並列サーバモデルを学ぶ。
 *
 * 全体アルゴリズム（プロセスモデル）：
 * 1) 親プロセスが listen ソケットで accept を繰り返す
 * 2) 新しい接続 acc を accept したら fork()
 *    - 子プロセス：listen ソケット(soc) を閉じ、acc で送受信して終了
 *    - 親プロセス：acc を閉じ、次の accept に戻る
 * 3) 子が終了すると SIGCHLD が親に飛ぶので、ハンドラで wait() して回収する
 *    - 回収しないと zombie（ゾンビプロセス）が溜まる
 *
 * このモデルの特徴：
 * - 長所：実装が直感的／各接続が別プロセスなので “状態の分離” が簡単
 * - 短所：接続数が増えると fork コスト・コンテキストスイッチで重くなる
 *        （多重化モデルの方がスケールしやすいことが多い）
 *
 * 注意（教材の重要ポイント）：
 * - fork 後に “どの FD を閉じるか” が重要
 *   - 子は listen FD を閉じる（不要、かつ親の accept を邪魔しない）
 *   - 親は acc を閉じる（子に任せる。親が持ち続けると切断検知が遅れる等）
vv * - SIGCHLD ハンドラ内での wait の扱い（複数同時終了・再入・安全性）は奥が深い
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

/* サーバソケットの準備（listen ソケットを作る）
 *
 * portnm: 待受ポート（文字列）
 *
 * 手順：
 * - getaddrinfo(NULL, port, AI_PASSIVE) で待受け用アドレスを得る
 * - socket → setsockopt(SO_REUSEADDR) → bind → listen
 */
int
server_socket(const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    /* hints の初期化 */
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */
    hints.ai_flags = AI_PASSIVE;      /* 待受け */

    /* アドレス解決 */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* 表示（学習用ログ） */
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

    /* 再利用フラグ（TIME_WAIT で bind 失敗しにくくする） */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* bind（待受けポートに割当） */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* listen（接続待ち開始） */
    if (listen(soc, SOMAXCONN) == -1) {
        perror("listen");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    freeaddrinfo(res0);
    return (soc);
}

/* アクセプトループ（fork 型並列サーバの中核）
 *
 * soc: listen ソケット FD（親プロセスが保持）
 *
 * アルゴリズム：
 * - 親：accept で新規接続 acc を得る
 * - fork()
 *   - 子：soc を close → acc で send_recv_loop → close(acc) → _exit
 *   - 親：acc を close（子が担当）→ 次の accept
 *
 * さらに：
 * - ここでは waitpid(-1, ..., WNOHANG) を “保険” で呼び、
 *   シグナルで回収し損ねた子がいないかを確認している
 *   （ただし、SIGCHLD ハンドラでも wait しているので “二重回収” の設計には注意）
 */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;
    int acc, status;
    pid_t pid;
    socklen_t len;

    for (;;) {
        len = (socklen_t) sizeof(from);

        /* 新規接続受付
         * - accept はブロッキングで待つ
         * - EINTR はシグナル割り込みなので、通常は再試行の対象
         */
        if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
            if (errno != EINTR) {
                perror("accept");
            }
        } else {
            /* 接続元アドレス表示（学習用ログ） */
            (void) getnameinfo((struct sockaddr *) &from, len,
                               hbuf, sizeof(hbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV);
            (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

            /* fork：接続ごとに子プロセスを作る */
            if ((pid = fork()) == 0) {
                /* ===== 子プロセス =====
                 * 子は “この acc を処理する担当”
                 */

                /* 子は listen ソケットを使わない（親が accept する）
                 * - 子が soc を持ち続けると、親が落ちた時にポート解放が遅れるなど
                 *   不要な副作用を生むので close するのが定石
                 */
                (void) close(soc);

                /* 送受信ループ（この接続だけを担当） */
                send_recv_loop(acc);

                /* 接続ソケットを閉じる */
                (void) close(acc);

                /* 子プロセス終了
                 * - exit() でも良いが、fork 後はバッファ二重フラッシュ等を避けるため
                 *   _exit() を使うのが典型
                 */
                _exit(1);

            } else if (pid > 0) {
                /* ===== 親プロセス =====
                 * 親は “次の accept を担当”
                 */

                /* 親は acc を使わない（子が処理する）
                 * - 親が acc を持ったままだと、子が close しても参照カウントが残り
                 *   相手から見た切断が遅れるなどの問題が起きる
                 */
                (void) close(acc);
                acc = -1;

            } else {
                /* fork 失敗：資源不足など */
                perror("fork");
                (void) close(acc);
                acc = -1;
            }

            /* “保険” の zombie 回収（WNOHANG）
             * - waitpid(-1, ..., WNOHANG) は “終了している子がいれば1つ回収”
             * - SIGCHLD ハンドラでも wait() しているので、設計としては
             *   「どちらか一方に統一」する方が分かりやすいことが多い
             */
            if ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                (void) fprintf(stderr,
                               "accept_loop:waitpid:pid=%d,status=%d\n",
                               pid, status);

                (void) fprintf(stderr,
                               "  WIFEXITED:%d,WEXITSTATUS:%d,WIFSIGNALED:%d,"
                               "WTERMSIG:%d,WIFSTOPPED:%d,WSTOPSIG:%d\n",
                               WIFEXITED(status),
                               WEXITSTATUS(status),
                               WIFSIGNALED(status),
                               WTERMSIG(status),
                               WIFSTOPPED(status),
                               WSTOPSIG(status));
            }
        }
    }
}

/* サイズ指定文字列連結（strlcat 相当）
 * - dst のサイズ上限 size を超えないように連結する
 */
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

/* 送受信ループ（子プロセスが接続1本に対して回し続ける）
 *
 * acc: accept で得た接続FD
 *
 * アルゴリズム：
 * - recv で受信
 *   - len==0 なら相手が close（EOF）
 * - ":OK\r\n" を付けて send
 *
 * ログに getpid() を入れているのが学習上ポイント：
 * - “接続ごとに別PIDで動いている” ことが可視化できる
 *
 * 注意：
 * - buf[len] = '\0' は len==sizeof(buf) のとき境界外になり得る
 *   → 教材としては recv を sizeof(buf)-1 にするのが安全
 */
void
send_recv_loop(int acc)
{
    char buf[512], *ptr;
    ssize_t len;

    for (;;) {
        /* 受信 */
        if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
            perror("recv");
            break;
        }
        if (len == 0) {
            (void) fprintf(stderr, "<%d>recv:EOF\n", getpid());
            break;
        }

        /* 文字列化（終端付与） */
        buf[len] = '\0';

        /* 改行除去してログを整形 */
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }
        (void) fprintf(stderr, "<%d>[client]%s\n", getpid(), buf);

        /* 応答作成 */
        (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
        len = (ssize_t) strlen(buf);

        /* 応答送信 */
        if ((len = send(acc, buf, (size_t) len, 0)) == -1) {
            perror("send");
            break;
        }
    }
}

/* 子プロセス終了（SIGCHLD）ハンドラ
 *
 * 目的：
 * - 子プロセスが終了したときに wait() して回収し、zombie を防ぐ
 *
 * 重要ポイント：
 * - SIGCHLD は “複数の子が同時に終わる” と 1回の通知でまとめて来ることがある
 *   → 実運用では waitpid(-1, &status, WNOHANG) をループして “回収し尽くす” のが定石
 *
 * このコードは wait(&status) を 1回だけ呼ぶため、
 * 短時間に子が複数終了すると回収漏れが起き、zombie が残る可能性がある。
 * （ただし accept_loop 側の waitpid(WNOHANG) が保険になっている設計）
 */
void
sig_chld_handler(int sig)
{
    int status;
    pid_t pid;

    /* wait：終了した子を1つ回収（ブロックする可能性がある）
     * - SIGCHLD ハンドラ内でのブロックは設計として注意が必要
     * - 教材として “終了回収が必要” を示す意図が強い実装
     */
    pid = wait(&status);

    (void) fprintf(stderr, "sig_chld_handler:wait:pid=%d,status=%d\n", pid, status);
    (void) fprintf(stderr,
                   "  WIFEXITED:%d,WEXITSTATUS:%d,WIFSIGNALED:%d,"
                   "WTERMSIG:%d,WIFSTOPPED:%d,WSTOPSIG:%d\n",
                   WIFEXITED(status),
                   WEXITSTATUS(status),
                   WIFSIGNALED(status),
                   WTERMSIG(status),
                   WIFSTOPPED(status),
                   WSTOPSIG(status));
}

int
main(int argc, char *argv[])
{
    int soc;

    /* 引数チェック */
    if (argc <= 1) {
        (void) fprintf(stderr, "server5 port\n");
        return (EX_USAGE);
    }

    /* 子プロセス終了シグナル（SIGCHLD）のハンドラ登録
     * - 子が終了するたびに SIGCHLD が親に届く
     * - ハンドラで wait して回収し、zombie を防ぐ
     */
    (void) signal(SIGCHLD, sig_chld_handler);

    /* listen ソケット作成 */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    (void) fprintf(stderr, "ready for accept\n");

    /* accept + fork のメインループ */
    accept_loop(soc);

    /* 形式上（通常ここには戻らない） */
    (void) close(soc);
    return (EX_OK);
}

/*
 * 学習の次の一歩（fork サーバの改善アイデア）
 *
 * 1) SIGCHLD ハンドラを “waitpid(-1, &status, WNOHANG) ループ” にする
 *    - まとめて複数終了しても確実に回収できる
 *
 * 2) accept_loop 内の waitpid(WNOHANG) と “二重回収” にならないよう整理する
 *    - ハンドラ側だけ、またはループ側だけ、どちらかに寄せると設計が明確
 *
 * 3) さらにスケールさせたいなら：
 *    - fork ではなく thread 版（pthread）や
 *    - 1プロセス多重化（epoll）へ
 *    - あるいは prefork（事前に子を立てて accept させる）
 *    といったモデル比較に進むと理解が深まる
 */
