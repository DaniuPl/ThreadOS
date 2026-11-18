/* sndtest: the phone's speaker and microphone, from a program
 *
 * The PC's calls - SYS64_SND_GET/SET, SYS64_SND_STREAM, SYS64_MIC - on
 * the ARM64 kernel's virtio-sound (kernelarm64/vsnd.c). Each argument
 * is a step:
 *
 *   info                         what the kernel says of the speaker,
 *                                its loudness and the microphone
 *   play:FRAMES:SEED:VOL:MUTE    the loudness and mute set (SND_SET),
 *                                then FRAMES stereo frames of a
 *                                pseudo-random sequence from SEED
 *   rec:FRAMES                   FRAMES recorded; said: how many came
 *                                silent first, and of the rest the
 *                                first bytes and a CRC-32, which
 *                                scripts/tests/t_arm64sound.py finds
 *                                in what the microphone was given
 *   busy                         a second player is turned away while
 *                                one plays, and a player that ends
 *                                without letting go lets go all the same
 *   vol:VOL                      the loudness alone (the phone's
 *                                Settings follow it)
 *   notify:TEXT                  a notification, which the phone's
 *                                shell sounds unless it is silent
 *   sleep:MS                     a pause
 *
 * The sequence is xorshift32, so the test computes what each sample
 * must be - after the loudness, as the kernel scales it: v * vol / 100,
 * rounded towards zero.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "usys64.h"

#define CHUNK 4800                  /* frames: a tenth of a second */

static uint32_t rng;

static uint32_t xorshift(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
	}
	return ~c;
}

static long stream(int op, const int16_t *frames, long n)
{
	return syscall64_3(SYS64_SND_STREAM, op, (int64_t)(uintptr_t)frames, n);
}

static int info(void)
{
	uint32_t s[4] = { 0 };
	struct {
		int32_t present, recording, owner, rate, channels;
		char name[56];
	} mi;
	memset(&mi, 0, sizeof(mi));
	syscall64(SYS64_SND_GET, (int64_t)(uintptr_t)s, 0);
	syscall64(SYS64_MIC, 0, (int64_t)(uintptr_t)&mi);
	printf("[sndtest] speaker %u volume %u muted %u steps %u\n",
	       s[0], s[1], s[2], s[3]);
	printf("[sndtest] microphone %d '%s' %d Hz %d channels\n",
	       mi.present, mi.name, mi.rate, mi.channels);
	return s[0] ? 0 : 1;
}

static int play(long frames, uint32_t seed, int vol, int mute)
{
	syscall64(SYS64_SND_SET, vol, mute);
	int16_t *buf = malloc(CHUNK * 4);
	long r = stream(0, 0, 0);
	if (r != 0 || !buf) {
		printf("[sndtest] play: begin %ld\n", r);
		return 1;
	}
	rng = seed;
	long done = 0;
	while (done < frames) {
		long n = frames - done < CHUNK ? frames - done : CHUNK;
		for (long i = 0; i < n * 2; i++)
			buf[i] = (int16_t)(xorshift() >> 16);
		r = stream(1, buf, n);
		if (r != n) {
			printf("[sndtest] play: write %ld of %ld\n", r, n);
			break;
		}
		done += n;
	}
	long e = stream(2, 0, 0);
	printf("[sndtest] played %ld frames of seed %u at %d%s, end %ld\n",
	       done, seed, vol, mute ? " muted" : "", e);
	free(buf);
	return done == frames && e == 0 ? 0 : 1;
}

static int rec(long frames)
{
	int16_t *buf = malloc((size_t)frames * 4);
	long r = syscall64(SYS64_MIC, 1, 0);
	if (r != 0 || !buf) {
		printf("[sndtest] rec: open %ld\n", r);
		return 1;
	}
	long done = 0;
	while (done < frames) {
		long n = frames - done < CHUNK ? frames - done : CHUNK;
		r = syscall64_3(SYS64_MIC, 2, (int64_t)(uintptr_t)(buf + done * 2),
		                n);
		if (r <= 0) {
			printf("[sndtest] rec: read %ld\n", r);
			break;
		}
		done += r;
	}
	syscall64(SYS64_MIC, 3, 0);
	/* the device's silence until the first samples arrive */
	long quiet = 0;
	while (quiet < done && !buf[quiet * 2] && !buf[quiet * 2 + 1])
		quiet++;
	const uint8_t *heard = (const uint8_t *)(buf + quiet * 2);
	size_t bytes = (size_t)(done - quiet) * 4;
	printf("[sndtest] recorded %ld frames, %ld silent first; heard ",
	       done, quiet);
	for (size_t i = 0; i < 32 && i < bytes; i++)
		printf("%02x", heard[i]);
	printf(" crc %08x\n", crc32(heard, bytes));
	free(buf);
	return done == frames ? 0 : 1;
}

static int busy(void)
{
	long first = stream(0, 0, 0);
	pid_t c = fork();
	if (c == 0)
		_exit(stream(0, 0, 0) == -2 ? 0 : 1);
	int st = 0;
	waitpid(c, &st, 0);
	int turned_away = WIFEXITED(st) && WEXITSTATUS(st) == 0;
	stream(2, 0, 0);
	/* a player ending without its end */
	c = fork();
	if (c == 0)
		_exit(stream(0, 0, 0) == 0 ? 0 : 1);
	waitpid(c, &st, 0);
	int child_had_it = WIFEXITED(st) && WEXITSTATUS(st) == 0;
	long again = stream(0, 0, 0);
	stream(2, 0, 0);
	printf("[sndtest] busy: first %ld, a second turned away %d, "
	       "the child played %d, free again %ld\n",
	       first, turned_away, child_had_it, again);
	return first == 0 && turned_away && child_had_it && again == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
	int bad = 0;
	setvbuf(stdout, 0, _IONBF, 0);
	for (int i = 1; i < argc; i++) {
		char *a = argv[i];
		if (!strcmp(a, "info")) {
			bad |= info();
		} else if (!strncmp(a, "play:", 5)) {
			long frames = strtol(a + 5, &a, 10);
			uint32_t seed = (uint32_t)strtoul(a + 1, &a, 10);
			int vol = (int)strtol(a + 1, &a, 10);
			int mute = (int)strtol(a + 1, &a, 10);
			bad |= play(frames, seed, vol, mute);
		} else if (!strncmp(a, "rec:", 4)) {
			bad |= rec(strtol(a + 4, 0, 10));
		} else if (!strcmp(a, "busy")) {
			bad |= busy();
		} else if (!strncmp(a, "vol:", 4)) {
			syscall64(SYS64_SND_SET, strtol(a + 4, 0, 10), -1);
			printf("[sndtest] volume %s\n", a + 4);
		} else if (!strncmp(a, "notify:", 7)) {
			long r = syscall64(SYS64_NOTIFY, (int64_t)(uintptr_t)"sndtest",
			                   (int64_t)(uintptr_t)(a + 7));
			printf("[sndtest] notified '%s': %ld\n", a + 7, r);
		} else if (!strncmp(a, "sleep:", 6)) {
			usleep((useconds_t)strtol(a + 6, 0, 10) * 1000);
		} else {
			printf("[sndtest] no step '%s'\n", a);
			bad = 1;
		}
	}
	return bad;
}
