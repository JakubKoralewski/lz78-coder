#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <ctype.h>

#if defined(_WIN32) || defined(_WIN64) || defined(WINDOWS)
#define SLASH "\\"
#else
#define SLASH "/"
#endif

void printHelp() {
        printf(
                "LZ78 (en/de)coder by Jakub Koralewski.\n\n"
                "Example usages:\n"
                "./code --en --lzw input_file output_file\n"
                "./code --de input_file output_file\n\n"
                "Parameters:\n"
                "--help\n display this thing\n"
                "Encode or decode (--encode by default):\n"
                "-e, --en, --encode encode\n"
                "-d, --de, --decode decode\n"
        );
}

/* djb2 with case-insensitivity added
 * http://www.cse.yorku.ca/~oz/hash.html
 * @author Daniel J. Bernstein circa 2000
 */
unsigned long hash(char* str) {
        unsigned long hash = 5381;
        int c;

        while ((c = tolower(*str++)))
                hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

        return hash;
}

#define HELP 6951207451432  // "--help"
#define ENCODE_VERY_SHORT 5861495 // "-e"
#define ENCODE_SHORT 6383110514 // "--en"
#define ENCODE 7569864807555437 // "--encode"
#define DECODE_VERY_SHORT 5861494 // "-d"
#define DECODE_SHORT 6383110472 // "--de"
#define DECODE 7569864757746755 // "--decode"

struct OPTIONS {
        bool isLZW;
        bool isDecoding;
        char* input_file;
        char* output_file;
};

#define NUM_SEQUENCES 254

struct Pair {
        unsigned char prefix_index;
        unsigned char value;
};

struct Dictionary {
        struct Pair array[NUM_SEQUENCES];

        /* Amount of space already taken. */
        unsigned char taken;
};

/* Finds longest sequence matching criteria in Dictionary dict.
 * @param dict
 * @param values Array of length of values_length
 * @param values_length
 */
unsigned char dictionary_find(struct Dictionary* dict, const unsigned char* values, unsigned char values_length) {
        for (int i = dict->taken - 1; i >= 0; i--) {
                struct Pair key_value = dict->array[i];

                if (values_length == 1 && key_value.prefix_index == 0xFF) {
                        // Single element search
                        if (key_value.value == values[0]) {
                                // successful
                                return i;
                        }
                } else {
                        // Multiple element search when current pair is not a single element
                        unsigned char j = values_length;
                        unsigned char current_index = i;
                        unsigned char next_link = 0;
                        bool found = true;
                        do {
                                j--;
                                if (dict->array[current_index].value != values[j]) {
                                        found = false;
                                        break;
                                }
                                next_link = dict->array[current_index].prefix_index;
                                if (next_link == 0xFF)
                                        break;

                                // "Recursion"
                                current_index = next_link;
                        } while (current_index != 0xFF);
                        if (found && (next_link != 0xFF || j == 0))
                                return i;
                }
        }
        return 0xFF;
}

struct Pair* dictionary_get(struct Dictionary* dict, unsigned char prefix_index) {
        if (prefix_index == 0xFF) {
                return NULL;
        }
        assert(prefix_index < dict->taken);
        return &dict->array[prefix_index];
}

unsigned char dictionary_add(struct Dictionary* dict, unsigned char prefix_index, unsigned char value) {
        if (dict->taken == NUM_SEQUENCES) {
                return 0xFF;
        }
        struct Pair new_pair = {.prefix_index = prefix_index, .value = value};
        dict->array[dict->taken] = new_pair;
        dict->taken++;
        return dict->taken - 1;
}

struct Dictionary dictionary_new() {
        struct Dictionary dict;
        for (int i = 0; i < NUM_SEQUENCES; i++) {
                struct Pair empty_pair = {.value = 0, .prefix_index = 0};
                dict.array[i] = empty_pair;
        }
        dict.taken = 0;
        return dict;
}

int main(int argc, char* argv[]) {
        struct OPTIONS options = {
                .isDecoding = false,
                .input_file = NULL,
                .output_file = NULL
        };
        assert(argc > 1);
        for (unsigned char i = 1; i < argc && i != 0xFF; i++) {
                char* arg = argv[i];
                unsigned long h = hash(arg);
                switch (h) {
                        case (unsigned long)HELP:
                                printHelp();
                                return -1;
                                break;
                        case (unsigned long)DECODE:
                        case (unsigned long)DECODE_SHORT:
                        case DECODE_VERY_SHORT:
                                options.isDecoding = true;
                                break;
                        case (unsigned long)ENCODE:
                        case (unsigned long)ENCODE_SHORT:
                        case ENCODE_VERY_SHORT:
                                options.isDecoding = false;
                                break;
                        default:
                                if (options.input_file == NULL) {
                                        options.input_file = arg;
                                } else if (options.output_file == NULL) {
                                        options.output_file = arg;
                                } else {
                                        printf("unknown option or too many unnamed arguments\n");
                                        printHelp();
                                        return -1;
                                }
                }
        }
        assert(options.input_file != NULL);
        if (options.output_file == NULL) {
                strcpy(options.output_file, options.input_file);
                strcat(options.input_file, ".out");
                printf(
                        "Output file was not provided. Creating file: \"%s\" in your working directory.\n",
                        options.output_file
                );
        }

        char cwd[256];
        assert(getcwd(cwd, sizeof(cwd)) != NULL);

        // Open input file
        FILE* input_file;
        char input_file_name[256];
        memcpy(input_file_name, cwd, 256);
        strcat(strcat(input_file_name, SLASH), options.input_file);
        input_file = fopen(input_file_name, "r+b");
        if (input_file == NULL) {
                printf("Error opening input file: \"%s\". Make sure it exists.\n", input_file_name);
                return -1;
        }

        // Open output file
        FILE* output_file;
        char* output_file_name = strcat(strcat(cwd, SLASH), options.output_file);
        output_file = fopen(output_file_name, "w+b");
        if (output_file == NULL) {
                printf("Error opening output file: \"%s\". Make sure it exists.\n", output_file_name);
                return -1;
        }

        // Create dict and read
        struct Dictionary dict = dictionary_new();
        unsigned char* buffer = malloc(NUM_SEQUENCES);
        long pos_before = ftell(input_file);

        // Reset cursor back to beginning
        fseek(input_file, 0, SEEK_SET);

        unsigned char prefix_length = 1;
        unsigned char last_found_prefix_index = 0;
        unsigned char* dict_length_memo = NULL;
        if (options.isDecoding) {
                dict_length_memo = malloc(NUM_SEQUENCES);
                memset(dict_length_memo, 1, NUM_SEQUENCES);
        }

        // Read file in chunks of sizeof(buffer)
        while (!feof(input_file)) {
                size_t num_read = fread(buffer, NUM_SEQUENCES, 1, input_file);
                assert(num_read <= NUM_SEQUENCES);
                long current_file_length = ftell(input_file) - pos_before;

                unsigned short i = 0;
                // Read the buffer
                do {
                        unsigned char prefix_index = 0;
                        if (options.isDecoding && buffer[i] != 0xFF) {
                                // Arrays should be 1-indexed
                                buffer[i]--;
                        } else if (!options.isDecoding) {
                                prefix_index = dictionary_find(&dict, &buffer[i], prefix_length);
                        }

                        if (options.isDecoding) {
                                if (buffer[i] == 0xFF) {
                                        // Doesn't exist yet
                                        dictionary_add(&dict, 0xFF, buffer[i + 1]);
                                        fwrite(&buffer[i + 1], sizeof(unsigned char), 1, output_file);
                                } else {
                                        unsigned char added_pos = dictionary_add(&dict, buffer[i], buffer[i + 1]);
                                        dict_length_memo[added_pos] += dict_length_memo[buffer[i]];
                                        struct Pair* whats_found = dictionary_get(&dict, added_pos);
                                        if (current_file_length - i - 1 == 0) {
                                                /* Last one should not have the one added above */
                                                dict_length_memo[added_pos]--;
                                                /* Also it shouldn't write what it just added, since it's */
                                                /* garbage with no value. */
                                                whats_found = dictionary_get(&dict, whats_found->prefix_index);
                                        }
                                        unsigned char throwaway = 0;
                                        for (int k = 0; k < dict_length_memo[added_pos]; ++k) {
                                                fwrite(&throwaway, sizeof(unsigned char), 1, output_file);
                                        }
                                        // Write in the same place
                                        fseek(output_file, -1, SEEK_CUR);
                                        unsigned char j = 0;
                                        /* The dictionary is built backwards, so the length
                                         * of the sequence needs to be known in advance. */
                                        do {
                                                fwrite(
                                                        &whats_found->value,
                                                        sizeof(unsigned char),
                                                        1,
                                                        output_file
                                                );
                                                whats_found = dictionary_get(&dict, whats_found->prefix_index);
                                                if (whats_found == NULL)
                                                        break;
                                                j++;
                                                fseek(output_file, -2, SEEK_CUR);
                                        } while (j < dict_length_memo[added_pos]);

                                        fseek(output_file, dict_length_memo[added_pos] - 1, SEEK_CUR);
                                }

                                i++;
                        } else if (prefix_index != 0xFF) {
                                // Found existing prefix, let's add to it by letting it iterate again
                                last_found_prefix_index = prefix_index;
                                if (i >= NUM_SEQUENCES - 1) {
                                        // If end of buffer when more is needed
                                        // increase buffer
                                        buffer = realloc(buffer, i + prefix_length - 1 + 2);
                                        // and read a single more byte
                                        fread(&buffer[i], 1, 1, input_file);
                                }
                                i--;
                                prefix_length++;
                        } else if (prefix_length > 1 && last_found_prefix_index != prefix_index) {
                                // Found new prefix
                                dictionary_add(
                                        &dict,
                                        last_found_prefix_index,
                                        buffer[i + prefix_length - 1]
                                );
                                if (i + prefix_length - 1 == current_file_length) {
                                        last_found_prefix_index++;
                                        fwrite(&last_found_prefix_index, sizeof(unsigned char), 1, output_file);
                                } else {
                                        fwrite(
                                                (unsigned char[])
                                                        {
                                                                last_found_prefix_index + 1,
                                                                buffer[i + prefix_length - 1]
                                                        },
                                                sizeof(unsigned char),
                                                2,
                                                output_file
                                        );
                                }
                                // Start looking for another prefix
                                i += prefix_length - 1;
                                prefix_length = 1;
                        } else if (prefix_length == 1) {
                                // Prefix found for the first time, let's write it
                                fwrite(
                                        (unsigned char[]) {prefix_index, buffer[i]},
                                        sizeof(unsigned char),
                                        2,
                                        output_file
                                );
                                dictionary_add(&dict, prefix_index, buffer[i]);

                                // Empty prefix cache
                                prefix_length = 1;
                        }

                        i++;

                        /* Assuming num_read == 0 is EOF, otherwise number of bytes read, then:
                         * If already end of file then read buffer until file cursor is exhausted,
                         * else: read the whole buffer. */
                } while (((num_read == 0 && current_file_length - i >= 0) || (num_read != 0 && num_read - i >= 0)));
        }
        printf("Saved output file to: \"%s\".\n", output_file_name);

        fclose(input_file);
        fclose(output_file);
        if (options.isDecoding) {
                free(dict_length_memo);
        }
        free(buffer);

        return 0;
}
