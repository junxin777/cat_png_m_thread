#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include "./starter/png_util/lab_png.h"
#include "./starter/png_util/zutil.h"

#define IMG_URL_1 "http://ece252-1.uwaterloo.ca:2520/image?img=1"
#define IMG_URL_2 "http://ece252-1.uwaterloo.ca:2520/image?img=2"
#define IMG_URL_3 "http://ece252-1.uwaterloo.ca:2520/image?img=3"

#define DUM_URL "https://example.com/"
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576 /* 1024*1024 = 1M */
#define BUF_INC 524288   /* 1024*512  = 0.5M */
#define MAX_FILE_NAME_LENGTH 100

int thread;
pthread_mutex_t mutex;
struct thread_data *ordered_files_data;

#define max(a, b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

// int fifty[50];

typedef struct recv_buf2
{
    unsigned char *buf; /* memory to hold a copy of received data */
    size_t size;        /* size of valid data in buf in bytes*/
    size_t max_size;    /* max capacity of buf in bytes*/
    int seq;            /* >=0 sequence number extracted from http header */
                        /* <0 indicates an invalid seq number */
} RECV_BUF;

// typedef struct {
//     char fileName[MAX_FILE_NAME_LENGTH];
//     int number;
// } FileNameInfo;

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
int write_file(const char *path, const void *in, size_t len);
void *thread_performence(void *arg);
int is_fifty(struct thread_data *fifty);
int catpng(struct thread_data *);

/**
 * @brief  cURL header call back function to extract image sequence number from
 *         http header data. An example header for image part n (assume n = 2) is:
 *         X-Ece252-Fragment: 2
 * @param  char *p_recv: header data delivered by cURL
 * @param  size_t size size of each memb
 * @param  size_t nmemb number of memb
 * @param  void *userdata user defined data structurea
 * @return size of header data received.
 * @details this routine will be invoked multiple times by the libcurl until the full
 * header data are received.  we are only interested in the ECE252_HEADER line
 * received so that we can extract the image sequence number from it. This
 * explains the if block in the code.
 */
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;

    if (realsize > strlen(ECE252_HEADER) &&
        strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0)
    {

        /* extract img sequence number */
        p->seq = atoi(p_recv + strlen(ECE252_HEADER));
    }
    return realsize;
}

/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv,
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;

    if (p->size + realsize + 1 > p->max_size)
    { /* hope this rarely happens */
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);
        char *q = realloc(p->buf, new_size);
        if (q == NULL)
        {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}

int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;

    if (ptr == NULL)
    {
        return 1;
    }

    p = malloc(max_size);
    if (p == NULL)
    {
        return 2;
    }

    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1; /* valid seq should be non-negative */
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL)
    {
        return 1;
    }

    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

/**
 * @brief output data in memory to a file
 * @param path const char *, output file path
 * @param in  void *, input data to be written to the file
 * @param len size_t, length of the input data in bytes
 */

int write_file(const char *path, const void *in, size_t len)
{
    FILE *fp = NULL;

    if (path == NULL)
    {
        fprintf(stderr, "write_file: file name is null!\n");
        return -1;
    }

    if (in == NULL)
    {
        fprintf(stderr, "write_file: input data is null!\n");
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == NULL)
    {
        perror("fopen");
        return -2;
    }

    if (fwrite(in, 1, len, fp) != len)
    {
        fprintf(stderr, "write_file: imcomplete write!\n");
        return -3;
    }
    return fclose(fp);
}

/// @brief
/// @param arg column number for each thread
/// @return NULL
void *thread_perform(void *arg)
{
    while (1)
    {
        pthread_mutex_lock(&mutex);
        if (is_fifty(ordered_files_data) == 0)
        {
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);

        CURL *curl_handle;
        CURLcode res;
        RECV_BUF recv_buf;
        char fname[256];
        pid_t pid = getpid();

        recv_buf_init(&recv_buf, BUF_SIZE);

        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_handle = curl_easy_init();
        if (curl_handle == NULL)
        {
            fprintf(stderr, "curl_easy_init: returned NULL\n");
            pthread_exit(NULL);
        }
        /* specify URL to get */
        curl_easy_setopt(curl_handle, CURLOPT_URL, ordered_files_data[0].server);

        /* register write call back function to process received data */
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3);
        /* user defined data structure passed to the call back function */
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&recv_buf);

        /* register header call back function to process received header data */
        curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl);
        /* user defined data structure passed to the call back function */
        curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)&recv_buf);

        /* some servers requires a user-agent field */
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

        /* get it! */
        res = curl_easy_perform(curl_handle);

        if (res != CURLE_OK)
        {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }
        else
        {
            printf("%lu bytes received in memory %p, seq=%d.\n",
                   recv_buf.size, recv_buf.buf, recv_buf.seq);
        }

        sprintf(fname, "./output_%d_%d.png", recv_buf.seq, pid);

        pthread_mutex_lock(&mutex);
        if (ordered_files_data[recv_buf.seq].seq != -1)
        {
            pthread_mutex_unlock(&mutex);
            curl_easy_cleanup(curl_handle);
            curl_global_cleanup();
            recv_buf_cleanup(&recv_buf);
            continue;
        }

        ordered_files_data[recv_buf.seq].buf = calloc(recv_buf.size, sizeof(char));
        memcpy(ordered_files_data[recv_buf.seq].buf, recv_buf.buf, recv_buf.size);
        ordered_files_data[recv_buf.seq].max_size = recv_buf.max_size;
        ordered_files_data[recv_buf.seq].seq = recv_buf.seq;
        ordered_files_data[recv_buf.seq].size = recv_buf.size;
        pthread_mutex_unlock(&mutex);

        /* cleaning up */
        curl_easy_cleanup(curl_handle);
        curl_global_cleanup();
        recv_buf_cleanup(&recv_buf);
    }

    pthread_exit(NULL);
}

int is_fifty(struct thread_data *fifty)
{
    for (size_t i = 0; i < 50; i++)
    {
        if (fifty[i].seq == -1)
        {
            return 1;
        }
    }
    return 0;
}

int catpng(struct thread_data *files_data)
{

    const int png_num = 50;

    int total_height = 0, buf_height = 0, buf_width = 0;
    unsigned long buf_uncompressed_data_size_total = 0;

    // 1. find total height
    // 2. find uncompressed total data buf size for IDAT
    for (int i = 0; i < png_num; i++)
    {
        struct data_IHDR *data_IHDR_create = calloc(1, sizeof(struct data_IHDR));

        get_png_data_IHDR(data_IHDR_create, files_data[i]);
        buf_width = get_png_width(data_IHDR_create);
        buf_height = get_png_height(data_IHDR_create);
        total_height += buf_height;

        free(data_IHDR_create);
        data_IHDR_create = NULL;
        // fclose(file);
    }
    int location = 0; // This is a coordination of writting umcompressed data

    // create buf stores UNZIPPED TOTAL data for IDAT
    buf_uncompressed_data_size_total = total_height * (buf_width * 4 + 1);
    U8 *buf_uncompressed_data = calloc(buf_uncompressed_data_size_total, sizeof(U8));

    // create buf stores zipped TOTAL data size for IDAT
    U64 buf_compressed_data_size_total = 0;

    for (int i = 0; i < png_num; i++)
    {
        // this size is unzipped data size for each png
        unsigned long buf_uncompressed_data_size_each = 0;
        int buf_height_each = 0;
        int buf_width_each = 0;

        // calculate each IDAT data size
        struct data_IHDR *data_IHDR = calloc(1, sizeof(struct data_IHDR));
        get_png_data_IHDR(data_IHDR, files_data[i]);
        buf_width_each = get_png_width(data_IHDR);
        buf_height_each = get_png_height(data_IHDR);
        buf_uncompressed_data_size_each = buf_height_each * (buf_width_each * 4 + 1);

        // create buf stores uncompressed EACH data buf size for IDAT
        U8 *data_buf_uncompressed = (U8 *)calloc(buf_uncompressed_data_size_each, sizeof(U8)); //*****************
        // U8 data_buf_uncompressed[buf_uncompressed_data_size_each]; //*****************

        struct chunk *chunk_IDAT = calloc(1, sizeof(struct chunk));
        get_chunk_IDAT(chunk_IDAT, files_data[i]);

        buf_compressed_data_size_total += chunk_IDAT->length;
        buf_uncompressed_data_size_each = 0;
        mem_inf(data_buf_uncompressed, &buf_uncompressed_data_size_each, chunk_IDAT->p_data, chunk_IDAT->length);

        for (int j = 0; j < buf_uncompressed_data_size_each; j++)
        {
            buf_uncompressed_data[location] = data_buf_uncompressed[j];
            location++;
        }

        free(data_IHDR);
        data_IHDR = NULL;
        free(chunk_IDAT->p_data);
        chunk_IDAT->p_data = NULL;
        free(chunk_IDAT);
        chunk_IDAT = NULL;
        free(data_buf_uncompressed);
        data_buf_uncompressed = NULL;
    }
    // so far what we've done
    // 1. total height calculated
    // 2. total unzipped data from IDAT stored in buf_uncompressed_data
    // 3. total zipped data size from IDAT stored in buf_compressed_data_size_total

    // create buf stores ZIPPED TOTAL data for IDAT
    U8 *buf_compressed_data = (U8 *)calloc(buf_compressed_data_size_total, sizeof(U8));

    // zipp the unzipped data which calculated in the second loop
    mem_def(buf_compressed_data, &buf_compressed_data_size_total, buf_uncompressed_data, buf_uncompressed_data_size_total, Z_DEFAULT_COMPRESSION);

    // create new header
    U32 header1 = 0x89504E47;
    U32 header2 = 0x0D0A1A0A;
    FILE *all = fopen("all.png", "wb"); // final png file
    // write new header to all.png
    header1 = htonl(header1);
    header2 = htonl(header2);
    fwrite(&header1, 4, 1, all);
    fwrite(&header2, 4, 1, all);

    // create new IHDR chunk from same png file ------------------------------------------------
    struct chunk *chunk_IHDR = NULL;
    chunk_IHDR = calloc(1, sizeof(struct chunk));
    chunk_IHDR->p_data = NULL;
    get_chunk_IHDR(chunk_IHDR, files_data[0]); // get IHDR info for all.png

    // data for IHDR
    U8 bit_depth = 0x08;
    U8 color = 0x06;
    U8 compress = 0x00;
    U8 filter = 0x00;
    U8 interlace = 0x00;
    buf_width = htonl(buf_width);
    total_height = htonl(total_height);
    memcpy(chunk_IHDR->p_data, &buf_width, 4);
    memcpy(chunk_IHDR->p_data + 4, &total_height, 4);
    memcpy(chunk_IHDR->p_data + 8, &bit_depth, 1);
    memcpy(chunk_IHDR->p_data + 9, &color, 1);
    memcpy(chunk_IHDR->p_data + 10, &compress, 1);
    memcpy(chunk_IHDR->p_data + 11, &filter, 1);
    memcpy(chunk_IHDR->p_data + 12, &interlace, 1);

    // calculate crc for new IHDR
    U32 new_crc_IHDR = get_crc(chunk_IHDR);

    // write IHDR length to all.png
    chunk_IHDR->length = htonl(chunk_IHDR->length);
    fwrite(&chunk_IHDR->length, 4, 1, all);

    // write IHDR type to all.png
    fwrite(&chunk_IHDR->type, 4, 1, all);

    // write IHDR data to all.png

    fwrite(chunk_IHDR->p_data, 13, 1, all);

    // write IHDR crc to all.png
    new_crc_IHDR = htonl(new_crc_IHDR);
    memcpy(&chunk_IHDR->crc, &new_crc_IHDR, 4);
    fwrite(&chunk_IHDR->crc, 4, 1, all);

    // create new IDAT chunk from same png file ------------------------------------------------
    chunk_p chunk_IDAT = calloc(1, sizeof(struct chunk));
    get_chunk_IDAT(chunk_IDAT, files_data[0]);

    // update legth for IDAT
    memcpy(&chunk_IDAT->length, &buf_compressed_data_size_total, 4);

    // update data for IDAT
    // we have to free p_data as when we generated a new chunk var, it initialized a p_data pointer
    // with randomly pointed data, we need to overwrite that data
    free(chunk_IDAT->p_data);
    chunk_IDAT->p_data = NULL;
    chunk_IDAT->p_data = buf_compressed_data;

    // calculate crc for new IDAT
    U32 new_crc_IDAT = get_crc(chunk_IDAT);

    // write IDAT length in all.png
    chunk_IDAT->length = htonl(chunk_IDAT->length);
    fwrite(&chunk_IDAT->length, 4, 1, all);

    // write IDAT type in all.png
    fwrite(&chunk_IDAT->type, 4, 1, all);

    // write IDAT data in all.png
    for (int i = 0; i < buf_compressed_data_size_total; i++)
    {
        fwrite(&chunk_IDAT->p_data[i], 1, 1, all);
    }

    // write IDAT crc in all.png
    new_crc_IDAT = htonl(new_crc_IDAT);
    memcpy(&chunk_IDAT->crc, &new_crc_IDAT, 4);
    fwrite(&chunk_IDAT->crc, 4, 1, all);

    // create new header
    U32 iend_length = 0x00000000;
    U32 iend_type = 0x49454E44;
    U32 iend_crc = 0xAE426082;
    // write new header to all.png
    iend_length = htonl(iend_length);
    iend_type = htonl(iend_type);
    iend_crc = htonl(iend_crc);
    fwrite(&iend_length, 4, 1, all);
    fwrite(&iend_type, 4, 1, all);
    fwrite(&iend_crc, 4, 1, all);


    free(chunk_IHDR->p_data);
    chunk_IHDR->p_data = NULL;
    free(chunk_IDAT->p_data);
    // free(chunk_IEND->p_data);
    free(chunk_IHDR);
    chunk_IHDR = NULL;
    free(chunk_IDAT);
    // free(chunk_IEND);
    free(buf_uncompressed_data);

    fclose(all);

    return 0;
}

int main(int argc, char **argv)
{
    int input;
    int t = 1;
    int n = 1;
    char *str = "option requires an argument";
    char *server = IMG_URL_1;
    pthread_mutex_init(&mutex, NULL);

    ordered_files_data = calloc(50, sizeof(struct thread_data));

    for (size_t i = 0; i < 50; i++)
    {
        ordered_files_data[i].seq = -1;
    }

    if (argc == 1)
    {
        fprintf(stderr, "%s: %s\n", argv[0], "Argument is not enough!");
    }

    while ((input = getopt(argc, argv, "t:n:")) != -1)
    {
        switch (input)
        {
        case 't':
            t = strtoul(optarg, NULL, 10);
            printf("option -t specifies a value of %d.\n", t);
            if (t <= 0)
            {
                fprintf(stderr, "%s: %s > 0 -- 't'\n", argv[0], str);
                return -1;
            }
            break;

        case 'n':
            n = strtoul(optarg, NULL, 10);
            printf("option -n specifies a value of %d.\n", n);
            if (n <= 0 || n > 3)
            {
                fprintf(stderr, "%s: %s 1, 2, or 3 -- 'n'\n", argv[0], str);
                return -1;
            }

            switch (n)
            {
            case 1:

                server = IMG_URL_1;

                printf("The selected server is %s\n", server);
                break;
            case 2:

                server = IMG_URL_2;

                printf("The selected server is %s\n", server);
                break;
            case 3:

                server = IMG_URL_3;

                printf("The selected server is %s\n", server);
                break;
            default:

                server = IMG_URL_1;

                printf("The selected server is %s\n", server);
                break;
            }
            break;

        default:
            fprintf(stderr, "%s: %s\n", argv[0], "Arguments are not correct!");
            return -1;
        }
    }

    for (size_t i = 0; i < 50; i++)
    {
        strcpy(ordered_files_data[i].server, server);
    }

    pthread_t *threads = malloc(t * sizeof(pthread_t));

    int thread_num;
    for (thread_num = 0; thread_num < t; thread_num++)
    {
        thread++;
        pthread_create(&threads[thread_num], NULL, thread_perform, NULL);
    }

    for (thread_num = 0; thread_num < t; thread_num++)
    {
        pthread_join(threads[thread_num], NULL);
    }

    for (size_t i = 0; i < 50; i++)
    {
        printf("ordered_files_data[%ld].seq = %d\n", i, ordered_files_data[i].seq);
        printf("ordered_files_data[%ld].size = %ld\n", i, ordered_files_data[i].size);
    }

    int cat_status = catpng(ordered_files_data);

    for (size_t i = 0; i < 50; i++)
    {
        free(ordered_files_data[i].buf);
    }
    free(ordered_files_data);
    free(threads);
    pthread_mutex_unlock(&mutex);
    pthread_mutex_destroy(&mutex);
    return (0);
}
