/*
 * SIGHUP を受けたら「自分自身を execve で上書き再実行」する TCP サーバ（re-exec server）。
 *
 * 目的：
 * - 通常の TCP サーバの基本形（listen → accept → recv/send）を理解する
 * - さらに、SIGHUP を契機にプロセスを “終了して起動し直す” のではなく、
 *   execve により “同じPIDのまま実行イメージを置き換える” ことを学ぶ
 *
 * このプログラムの 2 本柱（アルゴリズム）：
 *
 * [A] TCP サーバとしてのアルゴリズム
 *   1) server_socket(port):
 *        getaddrinfo → socket → setsockopt(SO_REUSEADDR) → bind → listen
 *   2) accept_loop(listen_fd):
 *        accept で接続確立 → 送受信(send_recv_loop) → close(conn_fd) を繰り返す（逐次処理）
 *   3) send_recv_loop(conn_fd):
 *        recv で受信 → 行末処理 → ":OK\r\n" を付けて send で返す
 *
 * [B] シグナルによる “自己再実行” アルゴリズム
 *   1) main で SIGHUP のハンドラ(sig_hangup_handler)を登録
 *   2) SIGHUP 到来でハンドラが割り込み実行される（非同期イベント）
 *   3) ハンドラが FD を整理（標準入出力以外を close）
 *   4) execve(argv[0], argv, envp) で自分自身を上書き再実行する
 *
 * execve の性質：
 * - 成功すると戻らない。現在のプロセスのメモリ空間は新しい実行イメージに差し替わり、
 *   新しい main() が最初から実行される。
 * - fork していないので PID は同一（“再起動”に似るが実体は “自己置換”）
 *
 * 重要な注意（教材上の簡略化ポイント）：
 * - signal handler 内の fprintf/perror は async-signal-safe ではない（厳密には危険）
 *   → 実務ではハンドラはフラグを立てるだけにし、メインループ側で安全に exec するのが定石
 * - SA_NODEFER により SIGHUP が再入可能（連打でハンドラが重なる可能性）
 *   → execve と再入は相性が悪いので、実務なら外すのが無難
 * - recv バッファ終端処理に境界外アクセスの可能性（len==512の場合）
 *   → 安全化するなら recv(..., sizeof(buf)-1, ...) が定石
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

/* accept_loop から send_recv_loop を呼ぶための前方宣言 */
void send_recv_loop(int acc);

/* クローズする最大ディスクリプタ値（簡易）
 *
 * - デーモン/サーバはFDを多く持つので exec 前に整理したい
 * - 本来は RLIMIT_NOFILE を参照すべきだが、教材として固定値を採用
 */
#define MAXFD   64

/* コマンドライン引数、環境変数のアドレス保持用（グローバル）
 *
 * - SIGHUP ハンドラ内で execve(argv[0], argv, envp) を呼びたい
 * - しかしハンドラは main のローカル変数に直接アクセスできない
 * - そこで argv/envp の “アドレス” を保持し、(*argv_)[0] のように間接参照する
 */
int *argc_;
char ***argv_;
char ***envp_;

/* SIGHUP ハンドラ
 *
 * sig: シグナル番号（SIGHUPなら通常 1）
 *
 * やっていること（アルゴリズム）：
 * 1) SIGHUP を受けたことを表示（教材用ログ）
 * 2) 標準入出力(0,1,2)を残して、それ以外のFDを close
 *    - 目的：exec 後に “不要なFD” を持ち越さない（FDリーク防止）
 *    - 例：listenソケット、accept済みソケット、ログファイル、パイプなどが残ると事故りやすい
 * 3) execve で自分自身を上書き再実行
 *
 * 注意：
 * - この実装は 3以上を全部閉じるため、listenソケットも閉じられる
 *   → accept中にSIGHUPが来ると accept/recv/send が EBADF 等で失敗する可能性がある
 * - 実務なら “安全なタイミングでリスタートする” ために、
 *   ハンドラではフラグを立ててループが落ち着いたところで close/exec する設計にすることが多い
 */
void
sig_hangup_handler(int sig)
{
    int i;

    /* NOTE: fprintf は async-signal-safe ではない（教材として簡略化） */
    (void) fprintf(stderr, "sig_hangup_handler(%d)\n", sig);

    /* stdin, stdout, stderr 以外をクローズ（3..MAXFD-1） */
    for (i = 3; i < MAXFD; i++) {
        (void) close(i);
    }

    /* 自プロセスの上書き再実行
     *
     * - 成功したらこの関数から戻らない（プロセスイメージが置換される）
     * - 失敗した場合のみ -1 が返り、perror が呼ばれる
     *
     * 注意：argv[0] が相対パス（例 "./re-exec"）だと、
     *       カレントディレクトリが変わった状況では execve が失敗し得る。
     */
    if (execve((*argv_)[0], (*argv_), (*envp_)) == -1) {
        /* NOTE: perror も async-signal-safe ではない（教材として簡略化） */
        perror("execve");
    }
}

/* サーバソケットの準備（portのみ指定）
 *
 * portnm: 待受ポート番号（文字列）
 * 戻り値: listen 済みソケットFD（成功） / -1（失敗）
 *
 * アルゴリズム（典型的TCPサーバのセットアップ）：
 * 1) getaddrinfo(NULL, port, hints) で bind 用 sockaddr を得る
 *    - host=NULL + AI_PASSIVE → サーバ待受用（典型的に 0.0.0.0）
 * 2) socket() でソケット生成
 * 3) setsockopt(SO_REUSEADDR) で再起動時の bind 失敗を減らす
 * 4) bind() でアドレス:ポートに束縛
 * 5) listen() で接続待ち状態へ
 */
int
server_socket(const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    /* hints をゼロクリアして必要項目だけ設定 */
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */
    hints.ai_flags = AI_PASSIVE;      /* 待受用（host=NULL時） */

    /* アドレス情報の決定（bind用sockaddrが得られる） */
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }

    /* デバッグ表示（実際に使うポートを数値表示） */
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

    /* SO_REUSEADDR を設定（TIME_WAIT の影響を受けにくくする） */
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* bind：アドレス:ポートを確定 */
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    /* listen：接続待ちキューを持つ状態へ */
    if (listen(soc, SOMAXCONN) == -1) {
        perror("listen");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    freeaddrinfo(res0);
    return (soc);
}

/* accept ループ（逐次処理）
 *
 * soc: listen ソケットFD
 *
 * アルゴリズム：
 * - accept で1接続を受け取る（接続専用FD acc を得る）
 * - 接続元を getnameinfo で表示
 * - send_recv_loop(acc) で通信
 * - close(acc)
 * - 次の accept へ
 *
 * 注意：
 * - 逐次サーバなので、1接続中は他接続を捌かない（同時接続を捌くなら fork/thread/epoll 等が必要）
 * - SIGHUP によって FD が閉じられると、accept が EBADF 等で失敗し得る（非同期割り込みの教材ポイント）
 */
void
accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;
    int acc;
    socklen_t len;

    for (;;) {
        len = (socklen_t) sizeof(from);

        /* 接続受付（シグナル割り込みで EINTR の可能性） */
        if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
            if (errno != EINTR) {
                perror("accept");
            }
        } else {
            (void) getnameinfo((struct sockaddr *) &from, len,
                               hbuf, sizeof(hbuf),
                               sbuf, sizeof(sbuf),
                               NI_NUMERICHOST | NI_NUMERICSERV);
            (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);

            /* 送受信ループ（1接続分） */
            send_recv_loop(acc);

            /* 接続FDをクローズ */
            close(acc);
            acc = 0;
        }
    }
}

/* サイズ指定文字列連結（strlcat相当）
 *
 * dst のバッファサイズ size を超えないように src を末尾に連結する。
 * 応答文字列 ":OK\r\n" を追加するときに利用。
 */
size_t
mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    /* dst の終端まで進む（最大 size まで） */
    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--);
    dlen = (size_t)(pd - dst);

    /* すでに満杯なら “本来必要だった長さ” を返す */
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }

    /* 終端NULを確保するため、書き込み可能最終位置は size-1 */
    pde = dst + size - 1;

    /* src を入るだけコピー */
    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    /* 残りを NUL で埋める（終端保証） */
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }

    /* src 長の計算（psは '\0' まで進める） */
    while (*ps++);
    return (dlen + (size_t)(ps - src - 1));
}

/* 送受信ループ（簡易 echo + OK）
 *
 * acc: accept で得た接続専用FD
 *
 * アルゴリズム：
 * 1) recv で受信
 * 2) len==0 なら相手が切断（EOF）→終了
 * 3) 受信データを文字列化して表示（CR/LFで行末処理）
 * 4) ":OK\r\n" を付けて send で返す
 *
 * 重要注意（境界）：
 * - recv の第三引数が sizeof(buf) なので、len が 512 になり得る
 * - buf[len] = '\0' は buf[512] となり境界外アクセスの可能性
 * - 安全化するなら recv(..., sizeof(buf)-1, ...) にする
 *
 * 重要注意（TCPの性質）：
 * - TCP はストリームなので、1回のrecvが「ちょうど1行」とは限らない
 * - 教材として単純化しているが、厳密には行バッファリングが必要
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
            (void) fprintf(stderr, "recv:EOF\n");
            break;
        }

        /* 文字列化（len==512の可能性がある点に注意） */
        buf[len] = '\0';

        /* CR/LF で切って表示（ログが1行になる） */
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }
        (void) fprintf(stderr, "[client]%s\n", buf);

        /* 応答文字列作成 */
        (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
        len = (ssize_t) strlen(buf);

        /* 応答送信（部分送信は未考慮：教材として単純化） */
        if ((len = send(acc, buf, (size_t) len, 0)) == -1) {
            perror("send");
            break;
        }
    }
}

/* main：SIGHUP で self re-exec する TCP サーバ
 *
 * argv[1] : port
 *
 * アルゴリズム：
 * 1) argv/envp のアドレスをグローバル保持（ハンドラで execve）
 * 2) sigaction で SIGHUP ハンドラ登録（SA_NODEFER）
 * 3) server_socket(port) で listen ソケット生成
 * 4) accept_loop で接続処理
 */
int
main(int argc, char *argv[], char *envp[])
{
    struct sigaction sa;
    int soc;

    /* 引数チェック */
    if (argc <= 1) {
        (void) fprintf(stderr, "re-exec port\n");
        return (EX_USAGE);
    }

    /* argv/envp をハンドラから参照できるように保持 */
    argc_ = &argc;
    argv_ = &argv;
    envp_ = &envp;

    /* SIGHUP ハンドラ設定（sigaction）
     *
     * 手順：
     * - 現在の設定を取得
     * - handler と flags を更新
     * - 設定適用
     *
     * 注意：
     * - sa.sa_mask を初期化していない（定番は sigemptyset(&sa.sa_mask)）
     * - sa.sa_flags を SA_NODEFER に上書きしている（元のフラグを捨てる）
     * - SA_NODEFER は再入を許し得るため、実務なら外すことが多い
     */
    (void) sigaction(SIGHUP, (struct sigaction *) NULL, &sa);
    sa.sa_handler = sig_hangup_handler;
    sa.sa_flags = SA_NODEFER;
    (void) sigaction(SIGHUP, &sa, (struct sigaction *) NULL);
    (void) fprintf(stderr, "sigaction():end\n");

    /* listen ソケット生成 */
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }

    (void) fprintf(stderr, "ready for accept\n");

    /* accept ループ（通常ここから戻らない） */
    accept_loop(soc);

    /* 念のため（通常到達しない） */
    (void) close(soc);
    return (EX_OK);
}

/*
 * 学習メモ（より安全な設計へ）
 *
 * 1) ハンドラ内で I/O をしない
 * - fprintf/perror は async-signal-safe ではない
 * - write(2) だけにするか、フラグを立てるだけにする
 *
 * 2) ハンドラ内で close/exec しない（タイミング問題）
 * - accept/recv/send の最中にFDを閉じると EBADF などが起きる
 * - フラグ方式で “安全な地点” で再起動するのが定石
 *
 * 3) recv のサイズを sizeof(buf)-1 にする
 * - buf[len]='\0' の境界外アクセスを防ぐ
 *
 * 4) SA_NODEFER を外す
 * - 再入を防いだ方が安全
 *
 * 5) MAXFD 固定をやめる
 * - RLIMIT_NOFILE を参照して全FDを確実に閉じる
 */
