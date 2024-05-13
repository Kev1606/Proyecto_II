#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

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
    FILE* archive = fopen(archiveName, "rb");
    if (archive == NULL) {
        fprintf(stderr, "Error al abrir el archivo empacado.\n");
        return;
    }

    FileAllocationTable fat;
    fread(&fat, sizeof(FileAllocationTable), 1, archive);

    printf("Contenido del archivo empacado:\n");
    printf("-------------------------------\n");

    for (size_t i = 0; i < fat.numFiles; i++) {
        FileMetadata entry = fat.files[i];
        printf("%s\t%zu bytes\n", entry.fileName, entry.fileSize);

        if (verbose) {
            printf("  Bloques: ");
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

void createArchive(const char *outputFile, char **inputFiles, int numInputFiles, bool verbose, bool veryVerbose, bool appendMode) {
    if (verbose) printf("Creando archivo %s\n", outputFile);
    FILE* archive = fopen(outputFile, appendMode ? "rb+" : "wb");

    if (archive == NULL) {
        fprintf(stderr, "Error al abrir el archivo %s\n", outputFile);
        exit(EXIT_FAILURE);
    }

    FileAllocationTable fat;
    memset(&fat, 0, sizeof(FileAllocationTable));

    if (!appendMode) {
        fat.freeBlocks[0] = sizeof(FileAllocationTable);
        fat.numFreeBlocks = 1;
        fwrite(&fat, sizeof(FileAllocationTable), 1, archive);
    } else {
        fread(&fat, sizeof(FileAllocationTable), 1, archive);
    }

    if (numInputFiles > 0 || appendMode) {
        for (int i = 0; i < numInputFiles; i++) {
            FILE* inputFile = fopen(inputFiles[i], "rb");
            if (inputFile == NULL) {
                fprintf(stderr, "Error al abrir el archivo %s\n", inputFiles[i]);
                continue;
            }

            if (verbose) printf("Agregando archivo %s\n", inputFiles[i]);
            size_t fileSize = 0;
            Block block;
            size_t bytesRead;

            while ((bytesRead = fread(&block, 1, sizeof(Block), inputFile)) > 0) {
                size_t blockPosition = findFreeBlock(&fat);
                if (blockPosition == (size_t)-1) {
                    if (veryVerbose) {
                        printf("No hay bloques libres, expandiendo el archivo\n");
                    }
                    expandArchive(archive, &fat);
                    blockPosition = findFreeBlock(&fat);
                    if (veryVerbose) {
                        printf("Nuevo bloque libre en la posición %zu\n", blockPosition);
                    }
                }

                if (bytesRead < sizeof(Block)) {
                    memset((char*)&block + bytesRead, 0, sizeof(Block) - bytesRead);
                }

                writeBlock(archive, &block, blockPosition);
                updateFAT(&fat, inputFiles[i], fileSize, blockPosition, bytesRead);

                fileSize += bytesRead;

                if (veryVerbose) {
                    printf("Escribiendo bloque %zu para archivo %s\n", blockPosition, inputFiles[i]);
                }
            }

            if (verbose) printf("Tamaño del archivo %s: %zu bytes\n", inputFiles[i], fileSize);

            fclose(inputFile);
        }
    } else {
        if (verbose) {
            printf("Leyendo datos desde la entrada estándar (stdin)\n");
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

            if (veryVerbose) {
                printf("Bloque %zu leído desde stdin y escrito en la posición %zu\n", blockPosition, blockPosition);
            }
        }
    }

    writeFAT(archive, &fat);
    fclose(archive);
}

void extractArchive(const char* archiveName, bool verbose, bool veryVerbose) {
    FILE* archive = fopen(archiveName, "rb");
    if (archive == NULL) {
        fprintf(stderr, "Error al abrir el archivo empacado.\n");
        return;
    }

    FileAllocationTable fat;
    fread(&fat, sizeof(FileAllocationTable), 1, archive);

    for (size_t i = 0; i < fat.numFiles; i++) {
        FileMetadata entry = fat.files[i];
        FILE* outputFile = fopen(entry.fileName, "wb");
        if (outputFile == NULL) {
            fprintf(stderr, "Error al crear el archivo de salida: %s\n", entry.fileName);
            continue;
        }

        if (verbose) {
            printf("Extrayendo archivo: %s\n", entry.fileName);
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
                printf("Bloque %zu del archivo %s extraído de la posición %zu\n", j + 1, entry.fileName, entry.blockPositions[j]);
            }
        }

        fclose(outputFile);
    }

    fclose(archive);
}

void deleteFilesFromArchive(const char* archiveName, char** fileNames, int numFiles, bool verbose, bool veryVerbose) {
    FILE* archive = fopen(archiveName, "rb+");
    if (archive == NULL) {
        fprintf(stderr, "Error al abrir el archivo empacado.\n");
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
                        printf("Bloque %zu del archivo '%s' marcado como libre.\n", fat.files[j].blockPositions[k], fileName);
                    }
                }

                for (size_t k = j; k < fat.numFiles - 1; k++) {
                    fat.files[k] = fat.files[k + 1];
                }
                fat.numFiles--;

                if (verbose) {
                    printf("Archivo '%s' eliminado del archivo empacado.\n", fileName);
                }

                break;
            }
        }

        if (!fileFound) {
            fprintf(stderr, "Archivo '%s' no encontrado en el archivo empacado.\n", fileName);
        }
    }

    fseek(archive, 0, SEEK_SET);
    fwrite(&fat, sizeof(FileAllocationTable), 1, archive);

    fclose(archive);
}

void updateFilesInArchive(const char* archiveName, char** fileNames, int numFiles, bool verbose, bool veryVerbose) {
    FILE* archive = fopen(archiveName, "rb+");
    if (archive == NULL) {
        fprintf(stderr, "Error al abrir el archivo empacado.\n");
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
                        printf("Bloque %zu del archivo '%s' marcado como libre.\n", fat.files[j].blockPositions[k], fileName);
                    }
                }

                FILE* inputFile = fopen(fileName, "rb");
                if (inputFile == NULL) {
                    fprintf(stderr, "Error al abrir el archivo de entrada: %s\n", fileName);
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
                        printf("Bloque %zu del archivo '%s' actualizado en la posición %zu\n", blockCount, fileName, blockPosition);
                    }
                }

                fat.files[j].fileSize = fileSize;
                fat.files[j].numBlocks = blockCount;

                fclose(inputFile);

                if (verbose) {
                    printf("Archivo '%s' actualizado en el archivo empacado.\n", fileName);
                }

                break;
            }
        }

        if (!fileFound) {
            fprintf(stderr, "Archivo '%s' no encontrado en el archivo empacado.\n", fileName);
        }
     }
     
     fseek(archive, 0, SEEK_SET);
     fwrite(&fat, sizeof(FileAllocationTable), 1, archive);
     
     fclose(archive);
}

void defragmentArchive(const char *archive_name, bool verbose, bool very_verbose) {
    FILE *archive = fopen(archive_name, "rb+");
    if (archive == NULL) {
        fprintf(stderr, "Error al abrir el archivo empacado.\n");
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
                printf("Bloque %zu del archivo '%s' movido a la posición %zu\n", j + 1, entry->fileName, entry->blockPositions[j]);
            }
        }

        entry->fileSize = file_size;

        if (verbose) {
            printf("Archivo '%s' desfragmentado.\n", entry->fileName);
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
        fprintf(stderr, "Error al abrir el archivo empacado.\n");
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
                printf("Bloque %zu leído desde stdin y agregado en la posición %zu\n", block_count, block_position);
            }
        }

        if (verbose) {
            printf("Contenido de stdin agregado al archivo empacado como '%s'.\n", filename);
        }
    } else {
        // Agregar archivos especificados
        for (int i = 0; i < num_files; i++) {
            const char *filename = filenames[i];
            FILE *input_file = fopen(filename, "rb");
            if (input_file == NULL) {
                fprintf(stderr, "Error al abrir el archivo de entrada: %s\n", filename);
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
                    printf("Bloque %zu del archivo '%s' agregado en la posición %zu\n", block_count, filename, block_position);
                }
            }

            fclose(input_file);

            if (verbose) {
                printf("Archivo '%s' agregado al archivo empacado.\n", filename);
            }
        }
    }

    // Escribir la estructura FAT actualizada en el archivo
    fseek(archive, 0, SEEK_SET);
    fwrite(&fat, sizeof(FileAllocationTable), 1, archive);

    fclose(archive);
}
     
int main(int argc, char *argv[]) {
    int opt;
    bool verbose = false;
    bool veryVerbose = false;
    bool create = false;
    bool extract = false;
    bool list = false;
    bool del = false;
    bool update = false;
    bool defrag = false;
    bool append = false;
    const char* outputFile = NULL;
    char** inputFiles = NULL;
    int numInputFiles = 0;

		while ((opt = getopt(argc, argv, "cxtduvrpf:")) != -1) {
				switch (opt) {
				    case 'c':
				        create = true;
				        break;
				    case 'x':
				        extract = true;
				        break;
				    case 't':
				        list = true;
				        break;
				    case 'd':
				        del = true;
				        break;
				    case 'u':
				        update = true;
				        break;
				    case 'v':
				        verbose = true;
				        break;
				    case 'r':
				        append = true;
				        break;
				    case 'p':
				        defrag = true;
				        break;
				    case 'f':
				        outputFile = optarg;
				        break;
				    default:
				        fprintf(stderr, "Uso: %s [-cxtduvrp] [-f archivo] [archivos...]\n", argv[0]);
				        exit(EXIT_FAILURE);
				}
		}
		
		optind = optind;
		
    if ((create || append) && outputFile == NULL && optind < argc) {
        outputFile = argv[optind++];
    }

    numInputFiles = argc - optind;
    if (numInputFiles > 0) {
        inputFiles = &argv[optind];
    }

    if (create) {
        createArchive(outputFile, inputFiles, numInputFiles, verbose, veryVerbose, false);
    } else if (extract) {
        extractArchive(outputFile, verbose, veryVerbose);
    } else if (list) {
        listArchiveContents(outputFile, verbose);
    } else if (del) {
        deleteFilesFromArchive(outputFile, inputFiles, numInputFiles, verbose, veryVerbose);
    } else if (update) {
        updateFilesInArchive(outputFile, inputFiles, numInputFiles, verbose, veryVerbose);
    } else if (defrag) {
        defragmentArchive(outputFile, verbose, veryVerbose);
    } else if (append) {
        appendFilesToArchive(outputFile, inputFiles, numInputFiles, verbose, veryVerbose);
    }

    return 0;
}
