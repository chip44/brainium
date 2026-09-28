#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define SIZE (1 << 18)
#define STRIDE 17
#define STEP_DELAY_US 100
#define PHASE_DELAY_US 1000000

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int *arr = malloc(SIZE * sizeof(int));
    if (!arr) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    printf("starting...\n");
    usleep(PHASE_DELAY_US);

    printf("sequential writes... ");
    for (int i = 0; i < SIZE; i++) {
        arr[i] = i;
    }
    printf("done\n");

    usleep(PHASE_DELAY_US);

    printf("strided reads... ");
    long sum = 0;
    int idx = 0;
    for (int n = 0; n < SIZE; n++) {
        sum += arr[idx];
        idx = (idx + STRIDE) % SIZE;
        usleep(STEP_DELAY_US);
    }
    printf("done (sum=%ld)\n", sum);

    usleep(PHASE_DELAY_US);

    printf("random writes... ");
    srand((unsigned int)time(NULL));
    for (int n = 0; n < SIZE; n++) {
        int r = rand() % SIZE;
        arr[r] = r * 2;
        usleep(STEP_DELAY_US);
    }
    printf("done\n");

    usleep(PHASE_DELAY_US);

    free(arr);

    return EXIT_SUCCESS;
}
