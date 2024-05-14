#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define BLOCK_SIZE 262144 // 256 KB
#define MAX_FILES 100
#define MAX_FILENAME_LENGTH 256
#define MAX_BLOCKS_PER_FILE 64
#define MAX_BLOCKS MAX_BLOCKS_PER_FILE * MAX_FILES

typedef struct {
    char fileName[MAX_FILENAME_LENGTH];
    size_t fileSize;
    size_t blockPositions[MAX_BLOCKS_PER_FILE];
    size_t numBlocks;
} FileMetadata;

struct Data {
    bool create;
    bool extract;
    bool list;
    bool delete;
    bool update;
    bool verbose;
    bool veryVerbose;
    bool file;
    bool append;
    bool defrag;
    char *outputFile;
    char **inputFiles;
    int numInputFiles;
};

typedef struct {
    FileMetadata files[MAX_FILES];
    size_t numFiles;
    size_t freeBlocks[MAX_BLOCKS];
    size_t numFreeBlocks;
} FileAllocationTable;

typedef struct {
    unsigned char data[BLOCK_SIZE];
} Block;

size_t findFreeBlock(FileAllocationTable* fat) {
    for (size_t i = 0; i < fat->numFreeBlocks; i++) {
        if (fat->freeBlocks[i] != 0) {
            size_t freeBlock = fat->freeBlocks[i];
            fat->freeBlocks[i] = 0;
            return freeBlock;
        }
    }
    return (size_t)-1;
}

void expandArchive(FILE* archive, FileAllocationTable* fat) {
    fseek(archive, 0, SEEK_END);
    size_t currentSize = ftell(archive);
    size_t expandedSize = currentSize + BLOCK_SIZE;
    ftruncate(fileno(archive), expandedSize);
    fat->freeBlocks[fat->numFreeBlocks++] = currentSize;
}

void listArchiveContents(const char* archiveName, bool verbose) {
		//printf("%s\n", archiveName);
    FILE* archive = fopen(archiveName, "rb");
    if (archive == NULL) {
        fprintf(stderr, "Error opening the packed file.\n");
        return;
    }

    FileAllocationTable fat;
    fread(&fat, sizeof(FileAllocationTable), 1, archive);

    printf("Contents of the packaged file:\n");
    printf("-------------------------------\n");

    for (size_t i = 0; i < fat.numFiles; i++) {
        FileMetadata entry = fat.files[i];
        printf("%s\t%zu bytes\n", entry.fileName, entry.fileSize);

        if (verbose) {
            printf("  Blocks: ");
            for (size_t j = 0; j < entry.numBlocks; j++) {
                printf("%zu ", entry.blockPositions[j]);
            }
            printf("\n");
        }
    }

    fclose(archive);
}

void writeBlock(FILE* archive, Block* block, size_t position) {
    fseek(archive, position, SEEK_SET);
    fwrite(block, sizeof(Block), 1, archive);
}

void updateFAT(FileAllocationTable* fat, const char* fileName, size_t fileSize, size_t blockPosition, size_t bytesRead) {
    for (size_t i = 0; i < fat->numFiles; i++) {
        if (strcmp(fat->files[i].fileName, fileName) == 0) {
            fat->files[i].blockPositions[fat->files[i].numBlocks++] = blockPosition;
            fat->files[i].fileSize += bytesRead;
            return;
        }
    }

    FileMetadata newEntry;
    strncpy(newEntry.fileName, fileName, MAX_FILENAME_LENGTH);
    newEntry.fileSize = fileSize + bytesRead;
    newEntry.blockPositions[0] = blockPosition;
    newEntry.numBlocks = 1;
    fat->files[fat->numFiles++] = newEntry;
}

void writeFAT(FILE* archive, FileAllocationTable* fat) {
    fseek(archive, 0, SEEK_SET);
    fwrite(fat, sizeof(FileAllocationTable), 1, archive);
}

void createArchive(struct Data data) {
    if (data.verbose) printf("Creating the file %s\n.......", data.outputFile);
    FILE* archive = fopen(data.outputFile, "wb");

    if (archive == NULL) {
        fprintf(stderr, "Error opening the file %s\n", data.outputFile);
        exit(EXIT_FAILURE);
    }

    FileAllocationTable fat;
    memset(&fat, 0, sizeof(FileAllocationTable));

    fat.freeBlocks[0] = sizeof(FileAllocationTable);
    fat.numFreeBlocks = 1;
    fwrite(&fat, sizeof(FileAllocationTable), 1, archive);

    if (data.numInputFiles > 0 && data.file) {
        for (int i = 0; i < data.numInputFiles; i++) {
            FILE* inputFile = fopen(data.inputFiles[i], "rb");
            if (inputFile == NULL) {
                fprintf(stderr, "Error opening the file %s\n", data.inputFiles[i]);
                continue;
            }

            if (data.verbose) printf("Adding file %s\n.......", data.inputFiles[i]);
            size_t fileSize = 0;
            Block block;
            size_t bytesRead;

            while ((bytesRead = fread(&block, 1, sizeof(Block), inputFile)) > 0) {
                size_t blockPosition = findFreeBlock(&fat);
                if (blockPosition == (size_t)-1) {
                    if (data.veryVerbose) {
                        printf("No free blocks, expanding the file\n");
                    }
                    expandArchive(archive, &fat);
                    blockPosition = findFreeBlock(&fat);
                    if (data.veryVerbose) {
                        printf("New free block in position %zu\n.......", blockPosition);
                    }
                }

                if (bytesRead < sizeof(Block)) {
                    memset((char*)&block + bytesRead, 0, sizeof(Block) - bytesRead);
                }

                writeBlock(archive, &block, blockPosition);
                updateFAT(&fat, data.inputFiles[i], fileSize, blockPosition, bytesRead);

                fileSize += bytesRead;

                if (data.veryVerbose) {
                    printf("Writing block %zu to file %s\n.......", blockPosition, data.inputFiles[i]);
                }
            }

            if (data.verbose) printf("File size %s: %zu bytes\n.......", data.inputFiles[i], fileSize);

            fclose(inputFile);
        }
    } else {
        if (data.verbose) {
            printf("Reading data from standard input (stdin)\n");
        }

        size_t fileSize = 0;
        Block block;
        size_t bytesRead;
        while ((bytesRead = fread(&block, 1, sizeof(Block), stdin)) > 0) {
            size_t blockPosition = findFreeBlock(&fat);
            if (blockPosition == (size_t)-1) {
                expandArchive(archive, &fat);
                blockPosition = findFreeBlock(&fat);
            }

            if (bytesRead < sizeof(Block)) {
                memset((char*)&block + bytesRead, 0, sizeof(Block) - bytesRead);
            }

            writeBlock(archive, &block, blockPosition);
            updateFAT(&fat, "stdin", fileSize, blockPosition, bytesRead);

            fileSize += bytesRead;

            if (data.veryVerbose) {
                printf("Block %zu read from stdin and written to position %zu\n", blockPosition, blockPosition);
            }
        }
    }

    writeFAT(archive, &fat);
    fclose(archive);
}

void extractArchive(const char* archiveName, bool verbose, bool veryVerbose) {
    FILE* archive = fopen(archiveName, "rb");
    if (archive == NULL) {
        fprintf(stderr, "Error opening packed file.\n");
        return;
    }

    FileAllocationTable fat;
    fread(&fat, sizeof(FileAllocationTable), 1, archive);

    for (size_t i = 0; i < fat.numFiles; i++) {
        FileMetadata entry = fat.files[i];
        FILE* outputFile = fopen(entry.fileName, "wb");
        if (outputFile == NULL) {
            fprintf(stderr, "Error creating output file: %s\n", entry.fileName);
            continue;
        }

        if (verbose) {
            printf("Extracting file: %s\n", entry.fileName);
        }

        size_t fileSize = 0;
        for (size_t j = 0; j < entry.numBlocks; j++) {
            Block block;
            fseek(archive, entry.blockPositions[j], SEEK_SET);
            fread(&block, sizeof(Block), 1, archive);

            size_t bytesToWrite = (fileSize + sizeof(Block) > entry.fileSize) ? entry.fileSize - fileSize : sizeof(Block);
            fwrite(&block, 1, bytesToWrite, outputFile);

            fileSize += bytesToWrite;

            if (veryVerbose) {
                printf("Block %zu of the file %s extracted from the position %zu\n", j + 1, entry.fileName, entry.blockPositions[j]);
            }
        }

        fclose(outputFile);
    }

    fclose(archive);
}

void deleteFilesFromArchive(const char* archiveName, char** fileNames, int numFiles, bool verbose, bool veryVerbose) {
    FILE* archive = fopen(archiveName, "rb+");
    if (archive == NULL) {
        fprintf(stderr, "Error opening packed file.\n");
        return;
    }

    FileAllocationTable fat;
    fread(&fat, sizeof(FileAllocationTable), 1, archive);

    for (int i = 0; i < numFiles; i++) {
        const char* fileName = fileNames[i];
        bool fileFound = false;

        for (size_t j = 0; j < fat.numFiles; j++) {
            if (strcmp(fat.files[j].fileName, fileName) == 0) {
                fileFound = true;

                for (size_t k = 0; k < fat.files[j].numBlocks; k++) {
                    fat.freeBlocks[fat.numFreeBlocks++] = fat.files[j].blockPositions[k];
                    if (veryVerbose) {
                        printf("Block %zu of file '%s' marked as free.\n", fat.files[j].blockPositions[k], fileName);
                    }
                }

                for (size_t k = j; k < fat.numFiles - 1; k++) {
                    fat.files[k] = fat.files[k + 1];
                }
                fat.numFiles--;

                if (verbose) {
                    printf("File '%s' removed from packed file.\n", fileName);
                }

                break;
            }
        }

        if (!fileFound) {
            fprintf(stderr, "File '%s' not found in packed file.\n", fileName);
        }
    }

    fseek(archive, 0, SEEK_SET);
    fwrite(&fat, sizeof(FileAllocationTable), 1, archive);

    fclose(archive);
}

void updateFilesInArchive(const char* archiveName, char** fileNames, int numFiles, bool verbose, bool veryVerbose) {
    FILE* archive = fopen(archiveName, "rb+");
    if (archive == NULL) {
        fprintf(stderr, "Error opening packed file.\n");
        return;
    }

    FileAllocationTable fat;
    fread(&fat, sizeof(FileAllocationTable), 1, archive);

    for (int i = 0; i < numFiles; i++) {
        const char* fileName = fileNames[i];
        bool fileFound = false;

        for (size_t j = 0; j < fat.numFiles; j++) {
            if (strcmp(fat.files[j].fileName, fileName) == 0) {
                fileFound = true;

                for (size_t k = 0; k < fat.files[j].numBlocks; k++) {
                    fat.freeBlocks[fat.numFreeBlocks++] = fat.files[j].blockPositions[k];
                    if (veryVerbose) {
                        printf("Block %zu of file '%s' marked as free.\n", fat.files[j].blockPositions[k], fileName);
                    }
                }

                FILE* inputFile = fopen(fileName, "rb");
                if (inputFile == NULL) {
                    fprintf(stderr, "Error opening input file: %s\n", fileName);
                    continue;
                }

                size_t fileSize = 0;
                size_t blockCount = 0;
                Block block;
                size_t bytesRead;
                while ((bytesRead = fread(&block, 1, sizeof(Block), inputFile)) > 0) {
                    size_t blockPosition = findFreeBlock(&fat);
                    if (blockPosition == (size_t)-1) {
                        expandArchive(archive, &fat);
                        blockPosition = findFreeBlock(&fat);
                    }

                    writeBlock(archive, &block, blockPosition);
                    fat.files[j].blockPositions[blockCount++] = blockPosition;

                    fileSize += bytesRead;

                    if (veryVerbose) {
                        printf("Block %zu of the file '%s' updated in position %zu\n", blockCount, fileName, blockPosition);
                    }
                }

                fat.files[j].fileSize = fileSize;
                fat.files[j].numBlocks = blockCount;

                fclose(inputFile);

                if (verbose) {
                    printf("File '%s' updated in the packed file.\n", fileName);
                }

                break;
            }
        }

        if (!fileFound) {
            fprintf(stderr, "File '%s' not found in packed file.\n", fileName);
        }
     }
     
     fseek(archive, 0, SEEK_SET);
     fwrite(&fat, sizeof(FileAllocationTable), 1, archive);
     
     fclose(archive);
}

void defragmentArchive(const char *archive_name, bool verbose, bool very_verbose) {
    FILE *archive = fopen(archive_name, "rb+");
    if (archive == NULL) {
        fprintf(stderr, "Error opening packed file.\n");
        return;
    }

    FileAllocationTable fat;
    fread(&fat, sizeof(FileAllocationTable), 1, archive);

    size_t new_block_position = sizeof(FileAllocationTable);
    for (size_t i = 0; i < fat.numFiles; i++) {
        FileMetadata *entry = &fat.files[i];
        size_t file_size = 0;

        for (size_t j = 0; j < entry->numBlocks; j++) {
            Block block;
            fseek(archive, entry->blockPositions[j], SEEK_SET);
            fread(&block, sizeof(Block), 1, archive);

            fseek(archive, new_block_position, SEEK_SET);
            fwrite(&block, sizeof(Block), 1, archive);

            entry->blockPositions[j] = new_block_position;
            new_block_position += sizeof(Block);
            file_size += sizeof(Block);

            if (very_verbose) {
                printf("Block %zu of file '%s' moved to position %zu\n", j + 1, entry->fileName, entry->blockPositions[j]);
            }
        }

        entry->fileSize = file_size;

        if (verbose) {
            printf("Defragmented '%s' file.\n", entry->fileName);
        }
    }

    // Actualizar la estructura FAT con los nuevos bloques libres
    fat.numFreeBlocks = 0;
    size_t remaining_space = new_block_position;
    while (remaining_space < BLOCK_SIZE * MAX_BLOCKS) {
        fat.freeBlocks[fat.numFreeBlocks++] = remaining_space;
        remaining_space += sizeof(Block);
    }

    // Escribir la estructura FAT actualizada en el archivo
    fseek(archive, 0, SEEK_SET);
    fwrite(&fat, sizeof(FileAllocationTable), 1, archive);

    // Truncar el archivo para eliminar el espacio no utilizado
    ftruncate(fileno(archive), new_block_position);

    fclose(archive);
}

void appendFilesToArchive(const char *archive_name, char **filenames, int num_files, bool verbose, bool very_verbose) {
    FILE *archive = fopen(archive_name, "rb+");
    if (archive == NULL) {
        fprintf(stderr, "Error opening packed file.\n");
        return;
    }
    FileAllocationTable fat;
    fread(&fat, sizeof(FileAllocationTable), 1, archive);

    if (num_files == 0) {
        // Leer desde la entrada estándar (stdin)
        char *filename = "stdin";
        size_t file_size = 0;
        size_t block_count = 0;
        size_t bytes_read = 0;
        Block block;
        while ((bytes_read = fread(&block, 1, sizeof(Block), stdin)) > 0) {
            size_t block_position = findFreeBlock(&fat);
            if (block_position == (size_t)-1) {
                expandArchive(archive, &fat);
                block_position = findFreeBlock(&fat);
            }

            writeBlock(archive, &block, block_position);
            updateFAT(&fat, filename, file_size, block_position, bytes_read);

            file_size += bytes_read;
            block_count++;

            if (very_verbose) {
                printf("Block %zu read from stdin and added at position %zu\n", block_count, block_position);
            }
        }

        if (verbose) {
            printf("Contents of stdin added to the packed file as '%s'.\n", filename);
        }
    } else {
        // Agregar archivos especificados
        for (int i = 0; i < num_files; i++) {
            const char *filename = filenames[i];
            FILE *input_file = fopen(filename, "rb");
            if (input_file == NULL) {
                fprintf(stderr, "Error opening input file: %s\n", filename);
                continue;
            }

            size_t file_size = 0;
            size_t bytes_read = 0;
            size_t block_count = 0;
            Block block;
            while ((bytes_read = fread(&block, 1, sizeof(Block), input_file)) > 0) {
                size_t block_position = findFreeBlock(&fat);
                if (block_position == (size_t)-1) {
                    expandArchive(archive, &fat);
                    block_position = findFreeBlock(&fat);
                }

                writeBlock(archive, &block, block_position);
                updateFAT(&fat, filename, file_size, block_position, bytes_read);

                file_size += bytes_read;
                block_count++;

                if (very_verbose) {
                    printf("Block %zu of the file '%s' added at position %zu\n", block_count, filename, block_position);
                }
            }

            fclose(input_file);

            if (verbose) {
                printf("File '%s' added to the packed file.\n", filename);
            }
        }
    }

    // Escribir la estructura FAT actualizada en el archivo
    fseek(archive, 0, SEEK_SET);
    fwrite(&fat, sizeof(FileAllocationTable), 1, archive);

    fclose(archive);
}
     
int main(int argc, char *argv[]) {
		
		// analisis de tiempo de ejecucion
		clock_t start, end;
    double cpu_time_used;
    
    // analisis de uso de memoria
    struct rusage r_usage;
    
    int opt;
    struct Data data = {
    			false,
    			false,
    			false,
    			false,
    			false,
    			false,
    			false,
    			false,
    			false,
    			false,
    			NULL,
    			NULL,
    			0
   	};

		while ((opt = getopt(argc, argv, "cxtduvwfrp:")) != -1) {
				switch (opt) {
				    case 'c':
				        data.create = true;
				        break;
				    case 'x':
				        data.extract = true;
				        break;
				    case 't':
				        data.list = true;
				        break;
				    case 'd':
				        data.delete = true;
				        break;
				    case 'u':
				        data.update = true;
				        break;
				    case 'v':
				    		if (data.verbose) {
				    			data.veryVerbose = true;
				    		}
				        data.verbose = true;
				        break;
				   	case 'f':
				        data.file = true;
				        break;
				    case 'r':
				        data.append = true;
				        break;
				    case 'p':
				        data.defrag = true;
				        break;
				    default:
				        fprintf(stderr, "Usage: %s [-cxtduvwfrp] [-f file] [files...]\n", argv[0]);
				        exit(EXIT_FAILURE);
				}
		}
		
		start = clock();
		
    if (optind < argc) {
        data.outputFile = argv[optind++];
    }

    data.numInputFiles = argc - optind;
    if (data.numInputFiles > 0) {
        data.inputFiles = &argv[optind];
    }

    if (data.create) {
        createArchive(data);
    } else if (data.extract) {
        extractArchive(data.outputFile, data.verbose, data.veryVerbose);
    } else if (data.delete) {
        deleteFilesFromArchive(data.outputFile, data.inputFiles, data.numInputFiles, data.verbose, data.veryVerbose);
    } else if (data.update) {
        updateFilesInArchive(data.outputFile, data.inputFiles, data.numInputFiles, data.verbose, data.veryVerbose);
    } else if (data.append) {
        appendFilesToArchive(data.outputFile, data.inputFiles, data.numInputFiles, data.verbose, data.veryVerbose);
    } else if (data.defrag) {
        defragmentArchive(data.outputFile, data.verbose, data.veryVerbose);
    } else if (data.list) {
        listArchiveContents(data.outputFile, data.verbose);
    }
    
    // Fin de la medición del tiempo de ejecución
    end = clock();
    cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
    printf("Tiempo de ejecución: %f segundos\n", cpu_time_used);
    
   	// Comienzo del análisis de uso de memoria
    getrusage(RUSAGE_SELF, &r_usage);
    printf("Memoria utilizada (en bytes): %ld\n", r_usage.ru_maxrss);
    // Fin del análisis de uso de memoria

    return 0;
}
