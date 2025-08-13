#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define LISTEN_PORT 5001
#define BACKLOG     1
#define OUTFILE     "/mnt/sdcard/received.txt"
#define BUF_SIZE    512

int main() {
    int sockfd, clientfd, outfd;
    struct sockaddr_in serv, cli;
    socklen_t cli_len = sizeof(cli);
    char buf[BUF_SIZE];
    ssize_t len;

    // 1) ソケット作成
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    // 2) サーバ情報設定
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;  // 全インターフェース待ち受け
    serv.sin_port = htons(LISTEN_PORT);

    // 3) バインド
    if (bind(sockfd, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("bind");
        close(sockfd);
        return EXIT_FAILURE;
    }

    // 4) リッスン開始
    if (listen(sockfd, BACKLOG) < 0) {
        perror("listen");
        close(sockfd);
        return EXIT_FAILURE;
    }
    printf("Listening on port %d...\n", LISTEN_PORT);

    // 5) クライアント接続受け入れ
    clientfd = accept(sockfd, (struct sockaddr *)&cli, &cli_len);
    if (clientfd < 0) {
        perror("accept");
        close(sockfd);
        return EXIT_FAILURE;
    }
    printf("Client connected: %s\n", inet_ntoa(cli.sin_addr));

    // 6) 出力ファイルを開く（追記モード）
    outfd = open(OUTFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (outfd < 0) {
        perror("open outfile");
        close(clientfd);
        close(sockfd);
        return EXIT_FAILURE;
    }

    // 7) データ受信ループ
    while ((len = read(clientfd, buf, BUF_SIZE)) > 0) {
        if (write(outfd, buf, len) != len) {
            perror("write file");
            break;
        }
    }
    if (len < 0) perror("read socket");

    // 8) クローズ
    close(outfd);
    close(clientfd);
    close(sockfd);
    printf("Done.\n");
    return EXIT_SUCCESS;
}

