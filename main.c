#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>

#define MAX_FILES 100
#define BLOCK_SIZE 512

typedef struct {
    char name[256];
    int size;
    int start_block;
} File;

typedef struct {
    File files[MAX_FILES];
    int num_files;
    char blocks[BLOCK_SIZE * 1024];
    int next_free_block;
} Archive;

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
} TarHeader;

void calculate_checksum(TarHeader *header) {
    unsigned int sum = 0;
    char *ptr = (char *)header;
    for (int i = 0; i < 512; i++) {
        sum += (unsigned char)ptr[i];
    }
    sprintf(header->checksum, "%07o", sum);
}

void create_tar_header(TarHeader *header, char *filename, struct stat st) {
    memset(header, 0, sizeof(TarHeader));
    strcpy(header->magic, "ustar");
    strcpy(header->version, "00");
    sprintf(header->mode, "%07o", st.st_mode & 0777);
    sprintf(header->uid, "%07o", st.st_uid);
    sprintf(header->gid, "%07o", st.st_gid);
    sprintf(header->size, "%011lo", (long)st.st_size);
    sprintf(header->mtime, "%011lo", (long)st.st_mtime);
    struct passwd *pw = getpwuid(st.st_uid);
    if (pw) strcpy(header->uname, pw->pw_name);
    struct group *gr = getgrgid(st.st_gid);
    if (gr) strcpy(header->gname, gr->gr_name);
    strncpy(header->name, filename, 100);
    header->type = '0';
    calculate_checksum(header);
}

void create_archive(char *filename, char *files[], int num_files, int verbose) {
    Archive archive = {0};
    FILE *fp = fopen(filename, "wb");
    
    for (int i = 0; i < num_files; i++) {
        File file = {0};
        strcpy(file.name, files[i]);
        
        struct stat st;
        stat(files[i], &st);
        file.size = st.st_size;
        
        TarHeader header;
        create_tar_header(&header, file.name, st);
        
        file.start_block = archive.next_free_block;
        fwrite(&header, sizeof(TarHeader), 1, fp);
        
        FILE *src = fopen(files[i], "rb");
        int bytes_read;
        while ((bytes_read = fread(archive.blocks + archive.next_free_block * BLOCK_SIZE, 1, BLOCK_SIZE, src)) > 0) {
            archive.next_free_block++;
            if (bytes_read < BLOCK_SIZE) {
                memset(archive.blocks + archive.next_free_block * BLOCK_SIZE, 0, BLOCK_SIZE - bytes_read);
                archive.next_free_block++;
            }
        }
        fclose(src);
        
        archive.files[archive.num_files++] = file;
        
        if (verbose) {
            printf("Agregado: %s (%d bytes)\n", file.name, file.size);
        }
    }
    
    int total_blocks = archive.next_free_block;
    fwrite(&archive.num_files, sizeof(int), 1, fp);
    fwrite(archive.files, sizeof(File), archive.num_files, fp);
    fwrite(archive.blocks, BLOCK_SIZE, total_blocks, fp);
    
    fclose(fp);
}


void extract_archive(char *filename, int verbose) {
    Archive archive = {0};
    FILE *fp = fopen(filename, "rb");
    
    fread(&archive.num_files, sizeof(int), 1, fp);
    fread(archive.files, sizeof(File), archive.num_files, fp);
    fread(archive.blocks, BLOCK_SIZE, 1024, fp);
    
    for (int i = 0; i < archive.num_files; i++) {
        File file = archive.files[i];
        
        if (verbose) {
            printf("Extrayendo: %s (%d bytes)\n", file.name, file.size);
        }
        
        FILE *dest = fopen(file.name, "wb");
        fwrite(archive.blocks + file.start_block * BLOCK_SIZE, 1, file.size, dest);
        fclose(dest);
    }
    
    fclose(fp);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Uso: %s [-c|-x] [-v] archivo [archivos...]\n", argv[0]);
        return 1;
    }
    
    int create = 0, extract = 0, verbose = 0;
    char *archive_file = NULL;
    char *files[MAX_FILES];
    int num_files = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            create = 1;
        } else if (strcmp(argv[i], "-x") == 0) {
            extract = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (archive_file == NULL) {
            archive_file = argv[i];
        } else {
            files[num_files++] = argv[i];
        }
    }
    
    if (create && extract) {
        printf("No se puede crear y extraer al mismo tiempo\n");
        return 1;
    }
    
    if (create) {
        create_archive(archive_file, files, num_files, verbose);
    } else if (extract) {
        extract_archive(archive_file, verbose);
    } else {
        printf("Debe especificar -c o -x\n");
        return 1;
    }
    
    return 0;
}
