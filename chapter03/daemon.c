/*
 * デーモン化（daemonize）の典型実装。
 *
 * 目的：
 * - 端末（TTY）やログインセッションから切り離された「常駐プロセス」を作る
 * - 親プロセス（起動元）を終了させ、バックグラウンドで動作するようにする
 * - カレントディレクトリや標準入出力を適切に処理し、意図せぬリソース保持を避ける
 *
 * デーモン化の標準アルゴリズム（このコードの核）：
 * 1) fork して親を終了（起動元シェルに制御を返す）
 * 2) setsid して新しいセッションを作る（端末から切り離す）
 * 3) SIGHUP を無視（端末切断時の影響を受けにくくする）
 * 4) もう一度 fork して「セッションリーダーでない」子にする
 *    → これにより、将来うっかり制御端末を再取得する可能性を潰す（重要）
 * 5) chdir("/") して実行ディレクトリを固定（マウント解除を妨げない）
 * 6) 開いているFDを閉じ、stdin/stdout/stderr を /dev/null に付け替える
 *
 * 引数：
 * - nochdir : 0なら chdir("/") する、1ならしない
 * - noclose : 0なら FD close と /dev/null 付け替えをする、1ならしない
 *
 * 注意（実務上の改善ポイント）：
 * - MAXFD=64 は固定値で雑。実際は getrlimit(RLIMIT_NOFILE) 等で上限を取得して閉じるのが一般的
 * - signal() より sigaction() を使う方が正確で推奨
 * - umask(0) を入れる実装も多い（ファイル作成権限を制御）
 * - syslog を使うのがデーモンの定番（標準出力は閉じるため）
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <unistd.h>

/* クローズする最大ディスクリプタ値（簡易版）
 *
 * 目的：
 * - デーモン化時に、親プロセスから引き継いだファイルディスクリプタ（FD）を全部閉じたい
 * - ただし本来は OS の FD 上限を取得してそれまで閉じるべき
 * - ここでは教材的に 0..63 を閉じる簡略化をしている
 */
#define MAXFD   64

/* デーモン化関数
 *
 * 戻り値：
 * - 成功: 0
 * - 失敗: -1（主に fork 失敗など）
 *
 * 重要：
 * - この関数が返った時点で、呼び出し元プロセスは「デーモンプロセス」になっている想定
 * - 親や最初の子は _exit() で終了している（stdioバッファを二重flushしないために _exit を使うのが定番）
 */
int
daemonize(int nochdir, int noclose)
{
    int i, fd;
    pid_t pid;

    /* 1回目 fork：
     * - 親（起動元）は終了
     * - 子だけが次へ進む
     *
     * 目的：
     * - 起動元シェル（端末）に制御を返し、バックグラウンド化の第一歩
     * - 親が生きたままだと、シェル側がジョブ制御で待ってしまうなどが起き得る
     */
    if ((pid = fork()) == -1) {
        /* fork失敗（リソース不足など） */
        return (-1);
    } else if (pid != 0) {
        /* 親プロセスは即終了（標準ライブラリの後処理を避けるため _exit） */
        _exit(0);
    }

    /* ここから「最初の子プロセス」 */

    /* setsid：
     * - 新しいセッションを作り、自分をセッションリーダーにする
     * - これにより制御端末（TTY）から切り離される
     * - 同時に「新しいプロセスグループのリーダー」にもなる
     *
     * デーモン化の要点：端末から分離して、ログアウト等の影響を受けにくくする
     */
    (void) setsid();

    /* SIGHUP 無視：
     * - 端末が切れたときにSIGHUPが飛ぶような環境で影響を受けにくくする
     * - 実務的には sigaction を使うのが推奨
     */
    (void) signal(SIGHUP, SIG_IGN);

    /* 2回目 fork：
     * - セッションリーダーのままだと、将来「制御端末を再取得できる」可能性が残る
     * - そこで、セッションリーダーでない子プロセスにするためもう一度 fork する
     *
     * 目的：
     * - デーモンが誤って端末を再び持ってしまうリスクを潰す（標準手順）
     */
    if ((pid = fork()) != 0) {
        /* 最初の子は終了（これで残るのは“孫”= デーモン本体） */
        _exit(0);
    }

    /* ここから「デーモンプロセス本体」 */

    /* カレントディレクトリ変更：
     * - デーモンが元の作業ディレクトリを保持していると、
     *   そのディレクトリを含むファイルシステムのアンマウント等を邪魔する可能性がある
     * - そのため通常は "/" に移動する（nochdir==0 のとき）
     */
    if (nochdir == 0) {
        (void) chdir("/");
    }

    /* ファイルディスクリプタ処理：
     * - 親プロセス（起動元）が開いていたFDを引き継いでいる可能性がある
     * - 例えば端末、パイプ、ソケット、ファイルなどを保持していると、
     *   意図しないリソース保持やセキュリティ問題になる
     * - そのため通常は全部閉じる（noclose==0 のとき）
     */
    if (noclose == 0) {

        /* すべてのFDを閉じる（簡易：0..MAXFD-1）
         * 実務では RLIMIT_NOFILE を見て上限まで閉じるのが一般的
         */
        for (i = 0; i < MAXFD; i++) {
            (void) close(i);
        }

        /* stdin/stdout/stderr を /dev/null に付け替える
         *
         * デーモンは端末に出力しないのが基本なので、
         * 標準入出力がどこにも向かないよう /dev/null にする。
         *
         * open("/dev/null", O_RDWR) で読み書き可能に開いて、
         * dup2 で fd 0,1,2 に複製する。
         */
        if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
            (void) dup2(fd, 0); /* stdin  */
            (void) dup2(fd, 1); /* stdout */
            (void) dup2(fd, 2); /* stderr */

            /* openしたfdが 0,1,2 以外なら不要なので閉じる */
            if (fd > 2) {
                (void) close(fd);
            }
        }
    }

    /* 成功 */
    return (0);
}

/* UNIT_TEST が定義されているときだけ main を含めてテスト実行できるようにする
 *
 * 例：
 *   gcc -DUNIT_TEST -o daemon_test daemon.c
 *
 * テスト内容（アルゴリズム確認）：
 * - daemonize(0,0) により
 *   - カレントディレクトリが "/" に移動しているか
 *   - 標準入出力が /dev/null に付け替わっているか
 * を確認する
 */
#ifdef UNIT_TEST
#include <syslog.h>

int
main(int argc, char *argv[])
{
    char buf[256];

    /* デーモン化：
     * - nochdir=0 なので "/" へ chdir する
     * - noclose=0 なのでFDを閉じ、stdin/out/err を /dev/null に付け替える
     */
    (void) daemonize(0, 0);

    /* ディスクリプタクローズのチェック：
     * - ここで fprintf(stderr, ...) しても、stderrは /dev/null に向いているため通常表示されない
     * - “何も表示されないこと”自体が正常動作の確認になる
     */
    (void) fprintf(stderr, "stderr\n");

    /* カレントディレクトリの表示：
     * - stdoutが無効化されているため、syslogでログに出すのがデーモン流
     * - getcwd で現在の作業ディレクトリを取得して syslog に書く
     */
    syslog(LOG_USER | LOG_NOTICE, "daemon:cwd=%s\n", getcwd(buf, sizeof(buf)));

    return (EX_OK);
}
#endif
