/* Copyright (C) 2017 Lorne Schell All rights reserved. */
/* This is a concise sigmoid nn over mnist in c for illustration/education purposes.
   Note that it's single core and doesn't have some basic features such as minibatch*/
/* Results:
 * 88.550% testing accuracy with
  static const int hidden_neurons = 64, output_neurons = 10;
  static const float initialization_sigma = 0.50;
  static const int epochs = 20;
  float learning_rate = 0.01f, decay = 1.001f;
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h> // for read
#include <float.h> // for FLT_MIN
#include <math.h> // for all the math functions
#include <stdint.h> // for uint32_t's
#include <stdio.h> // for putchar, fprintf
#include <stdlib.h> // for rand
#include <string.h> // for strerror
#include <sys/mman.h> // for mmap
#include <sys/stat.h> // for open
#include <unistd.h> // for read

typedef int FD;
typedef unsigned char uchar;
typedef int retcode;
static const retcode SUCCESS = 0;
static const retcode FAIL = -1;
#define retfail(CONDITION) { retcode __retval = CONDITION ; if(__retval == FAIL) { return __retval; } }

typedef struct matrix {
  size_t x, y;
  float *e;
} matrix;
struct neural_layer {
  matrix weights, w_delt, biases, b_delt, activations;
  void(*activate)(matrix activations, matrix biases);
  float(*backprop)(float weight);
};
struct dataset {
  uint32_t images, rows, columns;
  uchar *pixels;
  uchar *labels;
};

// swap the endianness, since this is on x86
retcode read_int(FD fp, uint32_t *r) {
  for (size_t i = 1; i <= sizeof(uint32_t); ++i) {
    retfail(read(fp, ((unsigned char *) r) + sizeof(uint32_t) - i, 1));
  }

  return SUCCESS;
}

// gunzip your mnist files beforehand - so we can mmap them here
retcode load_mnist(struct dataset *dat, char *image_file_name, char *label_file_name) {
  FD image_file = open(image_file_name, O_RDONLY);
  FD label_file = open(label_file_name, O_RDONLY);

  if (image_file == FAIL || label_file == FAIL) {
    fprintf(stderr, "%s:%d: Could not open the datafiles\n", __FILE__, __LINE__);
    perror(NULL);
    return FAIL;
  }

  uint32_t magic_num = 0;
  retfail(read_int(image_file, &magic_num));

  if (magic_num != 2051) {
    fprintf(stderr, "%s:%d: Image file magic num doesn't match %d\n", __FILE__, __LINE__, magic_num);
    return FAIL;
  }

  retfail(read_int(image_file, &dat->images));
  retfail(read_int(image_file, &dat->rows));
  retfail(read_int(image_file, &dat->columns));
  // for some reason, if I include the offset in the mmap call, it fails.
  dat->pixels = mmap(NULL, dat->images * dat->rows * dat->columns, PROT_READ, MAP_PRIVATE, image_file,
                     0) + sizeof(int32_t) * 4;

  if (dat->pixels == MAP_FAILED) {
    fprintf(stderr, "%s:%d: Map failed (%d): %s\n", __FILE__, __LINE__, errno, strerror(errno));
    return FAIL;
  }

  retfail(read_int(label_file, &magic_num));

  if (magic_num != 2049) {
    fprintf(stderr, "%s:%d: Image label magic num doesn't match %d\n", __FILE__, __LINE__, magic_num);
    return FAIL;
  }

  uint32_t label_count = 0;
  retfail(read_int(label_file, &label_count));

  if (dat->images != label_count) {
    fprintf(stderr, "%s:%d: Data count doesn't match labels %d and images %d\n", __FILE__, __LINE__, dat->images,
            label_count);
    return FAIL;
  }

  dat->labels = mmap(0, dat->images, PROT_READ, MAP_PRIVATE, label_file, 0) + sizeof(int32_t) * 2;

  if (dat->labels == MAP_FAILED) {
    fprintf(stderr, "%s:%d: Map failed (%d): %s\n", __FILE__, __LINE__, errno, strerror(errno));
    return FAIL;
  }

  return SUCCESS;
}
// Don't show the gradient, just show the values
retcode print_mnist_char(struct dataset *dat, int i) {
  for (int j = 0; j < 28; ++j) {
    for (int k = 0; k < 28; ++k) {
      if (dat->pixels[i * (dat->columns * dat->rows) + j * dat->columns + k] > 0) {
        putchar('X');

      } else {
        putchar(' ');
      }
    }

    putchar('\n');
  }

  putchar('\n');
  return SUCCESS;
}
void matrix_zero(struct matrix mat) {
  memset(mat.e, 0, mat.x * mat.y * sizeof(float));
}
void matmul(struct matrix a, struct matrix b, struct matrix result) {
  assert(a.y == b.x);
  assert(result.x == a.x);
  assert(result.y == b.y);

  for (size_t i = 0; i < a.x; i++) {
    for (size_t j = 0; j < a.y; j++) {
      for (size_t k = 0; k < b.y; k ++) {
        result.e[i * result.y + k] += a.e[i * a.y + j] * b.e[j * b.y + k];
      }
    }
  }
}
// a * T(b)
void matmulT(struct matrix a, struct matrix b, struct matrix result) {
  assert(a.y == b.y);
  assert(result.x == a.x);
  assert(result.y == b.x);

  for (size_t i = 0; i < a.x; i++) {
    for (size_t j = 0; j < a.y; j++) {
      for (size_t k = 0; k < b.x; k ++) {
        result.e[i * result.y + k] += a.e[i * a.y + j] * b.e[k * b.y + j];
      }
    }
  }
}
// T(a) * b
void Tmatmul(struct matrix a, struct matrix b, struct matrix result) {
  assert(a.x == b.x);
  assert(result.x == a.y);
  assert(result.y == b.y);

  for (size_t i = 0; i < a.y; i++) {
    for (size_t j = 0; j < a.x; j++) {
      for (size_t k = 0; k < b.y; k ++) {
        result.e[i * result.y + k] += a.e[j * a.y + i] * b.e[j * b.y + k];
      }
    }
  }
}
// gaussian distribution with a standard deviation of sigma and an average of mu
// generated using mox-muller transform
void randomize(float *data, size_t count, float sigma) {
  static const float two_pi = 2.0 * 3.14159265358979323846;
  static const float mu = 0.0;

  for (size_t i = 0; i < count; i += 2) {
    float u1, u2;

    do {
      u1 = (float) rand() * (1.0f / RAND_MAX);
      u2 = (float) rand() * (1.0f / RAND_MAX);
    } while (u1 <= FLT_MIN);

    data[i] = sqrtf(-2.0 * logf(u1)) * cosf(two_pi * u2) * sigma + mu;

    if (i + 1 < count) {
      data[i + 1] = sqrtf(-2.0 * logf(u1)) * sinf(two_pi * u2) * sigma + mu;
    }
  }
}
// fisher-yates shuffle
void shuffle(int *data, const size_t count) {
  for (int i = count - 1; i > 0; --i) {
    int j = 0;

    do { // clip the top to make uniform random
      j = rand();
    } while (j > RAND_MAX - (RAND_MAX % i));

    j = j % i;
    // swap
    data[i] ^= data[j];
    data[j] ^= data[i];
    data[i] ^= data[j];
  }
}
void sigmoid_activate(matrix activations, matrix biases) {
  assert(activations.x == 1);
  assert(activations.y == biases.y);

  for (int i = 0; i < activations.y; ++i) {
    float sum_term = activations.e[i] + biases.e[i];

    if (sum_term > 0) { // minimize numerical error
      activations.e[i] = 1.0f / (1.0 + expf(-1 * sum_term));

    } else {
      float z = exp(sum_term);
      activations.e[i] = z / (1 + z);
    }
  }
}
float sigmoid_deriv(float result) {
  return result * (1.0f - result);
}
int feedforward(struct neural_layer mnist_layers[], const int neural_layers) {
  // propigate the layers
  for (int j = 1; j < neural_layers; ++j) {
    matrix_zero(mnist_layers[j].activations);
    matmul(mnist_layers[j - 1].activations, mnist_layers[j].weights, mnist_layers[j].activations);
    mnist_layers[j].activate(mnist_layers[j].activations, mnist_layers[j].biases);
  }

  // check the predicted label
  int max = 0;
  const int last = neural_layers - 1;

  for (int h = 1; h < mnist_layers[last].activations.y; ++h) {
    if (mnist_layers[last].activations.e[h] > mnist_layers[last].activations.e[max]) {
      max = h;
    }
  }

  return max;
}
retcode mnist_neural_network_process() {
  static const int hidden_neurons = 30, output_neurons = 10;
  static const float initialization_sigma = 0.50;
  static const int epochs = 10;
  float learning_rate = 0.01f, decay = 1.001f;

  fprintf(stdout, "Loading training data...\n");
  struct dataset dat = { 0 }, test = { 0 };
  retfail(load_mnist(&dat, "train-images-idx3-ubyte", "train-labels-idx1-ubyte"));
  retfail(load_mnist(&test, "t10k-images-idx3-ubyte", "t10k-labels-idx1-ubyte"));

  fprintf(stdout, "Initializing sigmoid net...\n");
  const int pixcount = dat.columns * dat.rows;
  struct neural_layer mnist_layers[] = {
    {
      .weights = { 0 }, .w_delt = { 0 }, .biases = { 0 }, .b_delt = { 0 },
      .activations = { .x = 1, .y = pixcount, .e = malloc(pixcount * sizeof(float)) },
      .activate = NULL, .backprop = NULL,
    }, {
      .weights = { .x = pixcount, .y = hidden_neurons, .e = malloc(pixcount *hidden_neurons * sizeof(float)) },
      .w_delt = { .x = pixcount, .y = hidden_neurons, .e = calloc(pixcount * hidden_neurons, sizeof(float)) },
      .biases = { .x = 1, .y = hidden_neurons, .e = malloc(hidden_neurons * sizeof(float)) },
      .b_delt = { .x = 1, .y = hidden_neurons, .e = calloc(hidden_neurons, sizeof(float)) },
      .activations = { .x = 1, .y = hidden_neurons, .e = calloc(hidden_neurons, sizeof(float)) },
      .activate = &sigmoid_activate,
      .backprop = &sigmoid_deriv,
    }, {
      .weights = { .x = hidden_neurons, .y = output_neurons, .e = malloc(hidden_neurons *output_neurons * sizeof(float)) },
      .w_delt = { .x = hidden_neurons, .y = output_neurons, .e = calloc(hidden_neurons * output_neurons, sizeof(float)) },
      .biases = { .x = 1, .y = output_neurons, .e = malloc(output_neurons * sizeof(float)) },
      .b_delt = { .x = 1, .y = output_neurons, .e = calloc(output_neurons, sizeof(float)) },
      .activations = { .x = 1, .y = output_neurons, .e = calloc(output_neurons, sizeof(float)) },
      .activate = &sigmoid_activate,
      .backprop = &sigmoid_deriv,
    },
  };
  static const int neural_layers = 3;

  for (int i = 1; i < neural_layers; ++i) {
    randomize(mnist_layers[i].weights.e, mnist_layers[i].weights.x * mnist_layers[i].weights.y, initialization_sigma);
    randomize(mnist_layers[i].biases.e, mnist_layers[i].biases.y, initialization_sigma);
  }

  int data_index[dat.images];

  for (int i = 0; i < dat.images; ++i) {
    data_index[i] = i;
  }

  fprintf(stdout, "Training sigmoid net...\n");

  for (int e = 0; e < epochs; ++e) {
    int correct_count = 0;

    for (int i = 0; i < dat.images; ++i) {
      // convert the data
      for (int j = 0; j < pixcount; ++j) {
        mnist_layers[0].activations.e[j] = (float) dat.pixels[data_index[i] * pixcount + j];
      }

      int prediction = feedforward(mnist_layers, neural_layers);

      if (prediction == dat.labels[data_index[i]]) {
        ++ correct_count;
      }

      static const int last = neural_layers - 1;

      for (int j = last; j > 0; --j) {
        if (j == last) { // define the loss
          for (int k = 0; k < mnist_layers[j].b_delt.y; ++ k) {
            mnist_layers[j].b_delt.e[k] = mnist_layers[j].activations.e[k];

            if (k == dat.labels[data_index[i]]) {
              mnist_layers[j].b_delt.e[k] -= 1.0f;
            }
          }

        } else { // propigate back the error
          matrix_zero(mnist_layers[j].b_delt);
          matmulT(mnist_layers[j + 1].b_delt, mnist_layers[j + 1].weights, mnist_layers[j].b_delt);
        }

        assert(mnist_layers[j].activations.y == mnist_layers[j].b_delt.y);

        for (int k = 0; k < mnist_layers[j].activations.y; ++k) {
          mnist_layers[j].b_delt.e[k] *= mnist_layers[j].backprop(mnist_layers[j].activations.e[k]);
          mnist_layers[j].biases.e[k] -= learning_rate * mnist_layers[j].b_delt.e[k];
        }

        matrix_zero(mnist_layers[j].w_delt);
        Tmatmul(mnist_layers[j - 1].activations, mnist_layers[j].b_delt, mnist_layers[j].w_delt);

        for (int k = 0; k < mnist_layers[j].weights.x * mnist_layers[j].weights.y; ++k) {
          mnist_layers[j].weights.e[k] -= learning_rate * mnist_layers[j].w_delt.e[k];
        }
      }
    }

    fprintf(stdout, "Epoch %d Training Accuracy: %.3f%%\n", e, (100.0 * correct_count) / dat.images);

    if (e < epochs) {
      learning_rate /= decay;
      shuffle(data_index, dat.images);
    }

    // Test against test set
    correct_count = 0;

    for (int i = 0; i < test.images; ++i) {
      // convert the data
      for (int j = 0; j < pixcount; ++j) {
        mnist_layers[0].activations.e[j] = (float) test.pixels[i * pixcount + j];
      }

      int prediction = feedforward(mnist_layers, neural_layers);

      if (prediction == test.labels[i]) {
        ++ correct_count;
      }

      if (i < 10 && e == epochs - 1) {
        print_mnist_char(&test, i);
        fprintf(stdout, "Predicted: %d, Labelled: %d\n", prediction, test.labels[i]);
      }
    }

    fprintf(stdout, "Testing Accuracy: %.3f%%\n", (100.0 * correct_count) / test.images);
  }

  free(mnist_layers[0].activations.e);

  for (int i = 1; i < neural_layers; ++i) {
    free(mnist_layers[i].weights.e);
    free(mnist_layers[i].w_delt.e);
    free(mnist_layers[i].biases.e);
    free(mnist_layers[i].b_delt.e);
    free(mnist_layers[i].activations.e);
  }

  return SUCCESS;
}
retcode main() {
  srand(0xDEADBEEF);
  return mnist_neural_network_process();
}
