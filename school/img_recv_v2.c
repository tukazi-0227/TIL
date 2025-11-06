// img_recv.c  (JPEG/PNGバイナリ受信用: 1接続→1ファイル保存)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define LISTEN_PORT 5002
#define BACKLOG     1
#define OUTDIR      "/mnt/sdcard"           // SDカードのマウント先に合わせて変更
#define BUF_SIZE    65536
#define LOGTXT      OUTDIR "/imgrecv_metrics.txt"

static const unsigned char PNG_SIG[8]  = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
static const unsigned char JPEG_SIG[3] = {0xFF,0xD8,0xFF};

static void fmt_tv(char *dst, size_t sz, const struct timeval *tv) {
    struct tm tmv;
    localtime_r(&tv->tv_sec, &tmv);
    snprintf(dst, sz, "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (long)tv->tv_usec);
}

static void make_path(char *dst, size_t dstsz, const char *ext, const struct timeval *tv_start) {
    struct tm tmv; localtime_r(&tv_start->tv_sec, &tmv);
    // マイクロ秒まで入れて同秒での衝突を回避
    snprintf(dst, dstsz, "%s/received_%04d%02d%02d_%02d%02d%02d_%06ld.%s",
             OUTDIR,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (long)tv_start->tv_usec,
             ext);
}

// short write / EINTR を吸収して「必ず count バイト書く」
static ssize_t write_all(int fd, const void *buf, size_t count) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;   // 割り込みなら再試行
            return -1;                      // それ以外のエラー
        }
        if (w == 0) continue;               // 理論上稀だがループ継続
        p += w;
        left -= (size_t)w;
    }
    return (ssize_t)count;
}

int main() {
    int sockfd = -1, clientfd = -1, outfd = -1;
    struct sockaddr_in serv = {0}, cli = {0};
    socklen_t cli_len = sizeof(cli);
    char outpath[256];
    unsigned char buf[BUF_SIZE];
    ssize_t len;
    long long total = 0;                    // 集計用は 64bit
    struct timeval t0 = {0}, t1 = {0};
    double sec = 0.0, mbps = 0.0;
    FILE *logf = NULL;
    char startstr[48], endstr[48];

    // 出力先ディレクトリ（無ければ作成、既存ならOK）
    mkdir(OUTDIR, 0775);

    // ソケット作成
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { perror("socket"); return EXIT_FAILURE; }
    int yes = 1; setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // バインド
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(LISTEN_PORT);
    if (bind(sockfd, (struct sockaddr *)&serv, sizeof(serv)) < 0) { perror("bind"); close(sockfd); return EXIT_FAILURE; }

    // リッスン
    if (listen(sockfd, BACKLOG) < 0) { perror("listen"); close(sockfd); return EXIT_FAILURE; }
    printf("Listening on port %d...\n", LISTEN_PORT);

    // 接続受け入れ
    clientfd = accept(sockfd, (struct sockaddr *)&cli, &cli_len);
    if (clientfd < 0) { perror("accept"); close(sockfd); return EXIT_FAILURE; }
    printf("Client connected: %s\n", inet_ntoa(cli.sin_addr));

    // 受信開始時刻
    gettimeofday(&t0, NULL);

    // 最初の受信（ヘッダ判定用）
    len = read(clientfd, buf, sizeof(buf));
    if (len <= 0) { perror("read first"); goto cleanup; }
    total += len;

    // 画像種別判定（先頭バイトだけで判断。不足時は bin）
    const char *ext = "bin";
    if (len >= 8 && memcmp(buf, PNG_SIG, 8) == 0) ext = "png";
    else if (len >= 3 && memcmp(buf, JPEG_SIG, 3) == 0) ext = "jpg";

    // 受信開始時刻ベースのファイル名
    make_path(outpath, sizeof(outpath), ext, &t0);
    printf("Saving to: %s\n", outpath);

    // 出力ファイルを開く（バイナリ・新規/上書き）
    outfd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) { perror("open outfile"); goto cleanup; }

    // 既に受けた分を書き込み（short write 吸収）
    if (write_all(outfd, buf, (size_t)len) < 0) { perror("write first"); goto cleanup; }

    // 残りをループで受信→書き込み
    while ((len = read(clientfd, buf, sizeof(buf))) > 0) {
        total += len;
        if (write_all(outfd, buf, (size_t)len) < 0) { perror("write"); goto cleanup; }
    }
    if (len < 0) { perror("read"); }

    // データをSDに確実に反映
    if (fsync(outfd) < 0) perror("fsync(outfd)");

    // 終了時刻
    gettimeofday(&t1, NULL);
    sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1000000.0;
    if (sec > 0) mbps = (total * 8.0) / (sec * 1e6);

    fmt_tv(startstr, sizeof(startstr), &t0);
    fmt_tv(endstr,   sizeof(endstr),   &t1);

    printf("Done. %lld bytes saved. Elapsed: %.6f s, Throughput: %.3f Mbps\n",
           total, sec, mbps);

    // ログ追記（開始・終了・ファイル名・サイズ）
    logf = fopen(LOGTXT, "a");
    if (logf) {
        // CSVに近い形（必要ならヘッダ行は手動で一度だけ追加）
        fprintf(logf,
            "file=%s, start=%s, end=%s, bytes=%lld, elapsed_s=%.6f, throughput_Mbps=%.3f, remote=%s\n",
            outpath, startstr, endstr, total, sec, mbps, inet_ntoa(cli.sin_addr));
        fflush(logf);
        if (fsync(fileno(logf)) < 0) perror("fsync(logf)");
        fclose(logf);
    } else {
        perror("open logtxt");
    }

cleanup:
    if (outfd   >= 0) close(outfd);
    if (clientfd>= 0) close(clientfd);
    if (sockfd  >= 0) close(sockfd);
    return 0;
}
