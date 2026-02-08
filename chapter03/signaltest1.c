/*
 * SIGINT（Ctrl+C）を受け取るまで “.” を1秒ごとに表示し続けるサンプル。
 *
 * 目的：
 * - シグナルハンドラで「安全に」メイン処理へ通知する基本形を学ぶ
 * - “非同期に割り込むイベント（SIGINT）” を、共有フラグ（sig_atomic_t）で扱う
 *
 * 全体アルゴリズム（流れ）：
 * 1) main で sigaction(SIGINT, ...) により SIGINT ハンドラを登録する
 * 2) メインループは g_gotsig が 0 の間、1秒ごとに "." を表示して待つ
 * 3) ユーザが Ctrl+C を押すと OS が SIGINT を配送し、ハンドラ sig_int_handler が実行される
 * 4) ハンドラは g_gotsig にシグナル番号を格納する（0→非0へ）
 * 5) メインループは g_gotsig!=0 を検知して抜け、"END" を表示して終了する
 *
 * 学習ポイント：
 * - signal handler の中では “できるだけ少ないこと” を行う（定石）
 *   → このコードは「フラグ（g_gotsig）を書き換えるだけ」にしている
 * - フラグは volatile sig_atomic_t を使う
 *   → シグナルハンドラとメイン処理の間で安全に共有しやすい型（原子的に読み書きできることが期待される）
 *
 * 注意（SA_NODEFER について）：
 * - 通常、同じシグナルはハンドラ実行中は自動でブロックされる（再入防止）
 * - SA_NODEFER を付けるとブロックされず、SIGINT が連続するとハンドラが再入し得る
 * - 今回のハンドラは “代入1回” なので再入しても致命的になりにくいが、
 *   実務では基本的に SA_NODEFER は付けないことが多い（再入が事故要因になりやすい）
 */

#include <signal.h>
#include <stdio.h>
#include <sysexits.h>
#include <unistd.h>

/* シグナル受信フラグ（グローバル）
 *
 * volatile:
 * - コンパイラ最適化で “ループ内で値が変わらない” と仮定されないようにする意図
 *
 * sig_atomic_t:
 * - シグナルハンドラからの読み書きが “原子的に” 行えることが期待される型
 * - これにより「ハンドラが値を書き換え中にメインが半端な値を読む」リスクを減らす
 *
 * 初期値 0 は「まだシグナルを受け取っていない」ことを表す。
 * SIGINT を受け取ると、値が SIGINT（通常 2）など非0に変化する。
 */
volatile sig_atomic_t g_gotsig = 0;

/* SIGINT ハンドラ（Ctrl+C 用）
 *
 * sig: 受け取ったシグナル番号（SIGINT）
 *
 * シグナルハンドラの設計原則：
 * - async-signal-safe でない関数（printf/fprintf/malloc など）を呼ばない
 * - 共有フラグを立てるなど、最小限の処理にする
 *
 * ここでは “受信した” ことをメインループに知らせるために、
 * グローバル変数にシグナル番号を代入するだけ。
 */
void
sig_int_handler(int sig)
{
    /* 受信したシグナル番号を保存（0→非0） */
    g_gotsig = sig;
}

int
main(int argc, char *argv[])
{
    struct sigaction sa;

    /* 現在の SIGINT の設定を取得（教材用）
     *
     * sigaction(signum, NULL, &sa) は “取得だけ” を意味する。
     * ここで取ってきた sa をベースに handler/flags を変更して再設定する流れ。
     *
     * 注意：
     * - 実務の定石では、sa を memset でゼロクリアして sigemptyset でマスクも初期化する。
     * - このコードは “現在設定をベースにする” という教材的な書き方になっている。
     */
    (void) sigaction(SIGINT, (struct sigaction *) NULL, &sa);

    /* ハンドラ関数を登録 */
    sa.sa_handler = sig_int_handler;

    /* SA_NODEFER を指定
     *
     * 意味：
     * - 通常はハンドラ実行中に同じ SIGINT はブロックされる（再入しにくい）
     * - SA_NODEFER を付けるとブロックされず、SIGINT が連続すると再入し得る
     *
     * このコードではハンドラが “代入のみ” なので再入でも壊れにくいが、
     * 一般には再入は避けることが多い。
     */
    sa.sa_flags = SA_NODEFER;

    /* 設定を反映 */
    (void) sigaction(SIGINT, &sa, (struct sigaction *) NULL);

    /* メインループ：SIGINTを受け取るまで待つ
     *
     * アルゴリズム：
     * - g_gotsig が 0 の間は 1秒ごとに "." を出力
     * - Ctrl+C で SIGINT が届くと、ハンドラが g_gotsig を非0にする
     * - それを検知したらループを抜けて終了
     *
     * 注意：
     * - ここでは sleep(1) を使っているため “ポーリング（定期チェック）” 型の待ち方
     * - よりシグナルらしく待つなら pause() や sigsuspend() を使う設計もある
     *   （ただし教材としてはこの方が分かりやすい）
     */
    while (g_gotsig == 0) {
        (void) fprintf(stderr, ".");
        (void) sleep(1);
    }

    /* ループ終了＝SIGINT を受信した */
    (void) fprintf(stderr, "\nEND\n");
    return (EX_OK);
}

/*
 * 発展（より “正しい” シグナル待ちの例の方向性）：
 *
 * 1) sa を明示初期化する
 *   - memset(&sa, 0, sizeof(sa));
 *   - sigemptyset(&sa.sa_mask);
 *   - sa.sa_flags は必要なものだけ足す（SA_RESTART 等）
 *
 * 2) ポーリングではなく pause/sigsuspend で待つ
 *   - “シグナルが来るまでブロックして待つ” ほうがCPUにも優しい
 *
 * 3) SA_NODEFER を外す
 *   - 再入が不要なら付けないのが安全側
 */
