#include <zlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include "cgranges.h"
#include "ketopt.h"
#include "kseq.h"
KSTREAM_INIT(gzFile, gzread, 0x10000)

#define BEDTK_VERSION "0.0-r8-dirty"

/***************
 * BED3 parser *
 ***************/

typedef struct {
	int32_t l;
	char *s;
} bed_rest1_t;

typedef struct {
	int64_t n, m;
	bed_rest1_t *a;
} bed_rest_t;

static char *parse_bed3b(char *s, int32_t *st_, int32_t *en_, char **r)
{
	char *p, *q, *ctg = 0;
	int32_t i, st = -1, en = -1;
	if (r) *r = 0;
	for (i = 0, p = q = s;; ++q) {
		if (*q == '\t' || *q == '\0') {
			int c = *q;
			*q = 0;
			if (i == 0) ctg = p;
			else if (i == 1) st = atol(p);
			else if (i == 2) {
				en = atol(p);
				if (r && c != 0) *r = q, *q = c;
			}
			++i, p = q + 1;
			if (i == 3 || c == '\0') break;
		}
	}
	*st_ = st, *en_ = en;
	return i >= 3? ctg : 0;
}

static char *parse_bed3(char *s, int32_t *st_, int32_t *en_)
{
	return parse_bed3b(s, st_, en_, 0);
}

static cgranges_t *read_bed3b(const char *fn, bed_rest_t *r)
{
	gzFile fp;
	cgranges_t *cr;
	kstream_t *ks;
	kstring_t str = {0,0,0};
	int64_t k = 0;
	fp = fn && strcmp(fn, "-")? gzopen(fn, "r") : gzdopen(0, "r");
	if (fp == 0) {
		fprintf(stderr, "ERROR: failed to open the input file\n");
		return 0;
	}
	ks = ks_init(fp);
	cr = cr_init();
	if (r) r->m = r->n = 0, r->a = 0;
	while (ks_getuntil(ks, KS_SEP_LINE, &str, 0) >= 0) {
		char *ctg, *rest;
		int32_t st, en;
		ctg = parse_bed3b(str.s, &st, &en, &rest);
		if (ctg) {
			cr_add(cr, ctg, st, en, k);
			if (r) {
				bed_rest1_t *p;
				if (r->n == r->m) {
					int64_t old_m = r->m;
					r->m = r->m? r->m<<1 : 16;
					r->a = (bed_rest1_t*)realloc(r->a, r->m * sizeof(bed_rest1_t));
					memset(&r->a[old_m], 0, (r->m - old_m) * sizeof(bed_rest1_t));
				}
				p = &r->a[r->n++];
				p->l = rest? str.l - (rest - str.s) : 0;
				if (rest) {
					p->s = (char*)malloc(p->l + 1);
					memcpy(p->s, rest, p->l);
					p->s[p->l] = 0;
				}
			}
			++k;
		}
	}
	if (k > INT32_MAX)
		fprintf(stderr, "WARNING: more than %d records; some functionality may not work!\n", INT32_MAX);
	free(str.s);
	ks_destroy(ks);
	gzclose(fp);
	return cr;
}

static cgranges_t *read_bed3(const char *fn)
{
	return read_bed3b(fn, 0);
}

/*********
 * Tools *
 *********/

int main_isec(int argc, char *argv[])
{
	cgranges_t *cr;
	ketopt_t o = KETOPT_INIT;
	int64_t m_b = 0, *b = 0, n_b;
	int c, merge = 0, full = 0;

	while ((c = ketopt(&o, argc, argv, 1, "mf", 0)) >= 0) {
		if (c == 'm') merge = 1;
		else if (c == 'f') full = 1;
	}

	if (argc - o.ind < 1 || (argc - o.ind < 2 && isatty(0))) {
		printf("Usage: bedtk isec [options] <loaded.bed> <streamed.bed>\n");
		printf("Options:\n");
		printf("  -f      print overlapping records in the second BED\n");
		printf("  -m      merge overlapping regions in the second BED\n");
		return 1;
	}

	cr = read_bed3(argv[o.ind]);
	assert(cr);
	cr_index2(cr, 1);

	if (merge) {
		int64_t i;
		cgranges_t *qr;
		qr = read_bed3(o.ind+1 < argc? argv[o.ind + 1] : 0);
		assert(qr);
		if (!cr_is_sorted(qr)) cr_sort(qr);
		cr_merge_pre_index(qr);
		for (i = 0; i < qr->n_r; ++i) {
			cr_intv_t *q = &qr->r[i];
			int32_t st1 = (int32_t)q->x, en1 = q->y, cov_st = 0, cov_en = 0;
			int64_t j;
			char *ctg = qr->ctg[q->x>>32].name;
			n_b = cr_overlap(cr, ctg, st1, en1, &b, &m_b);
			for (j = 0; j < n_b; ++j) {
				cr_intv_t *r = &cr->r[b[j]];
				int32_t st0 = cr_st(r), en0 = cr_en(r);
				if (st0 < st1) st0 = st1;
				if (en0 > en1) en0 = en1;
				if (st0 > cov_en) {
					if (cov_en > cov_st) printf("%s\t%d\t%d\n", ctg, cov_st, cov_en);
					cov_st = st0, cov_en = en0;
				} else cov_en = cov_en > en0? cov_en : en0;
			}
			if (cov_en > cov_st) printf("%s\t%d\t%d\n", ctg, cov_st, cov_en);
		}
		cr_destroy(qr);
	} else {
		gzFile fp;
		kstream_t *ks;
		kstring_t str = {0,0,0};
		fp = o.ind+1 < argc && strcmp(argv[o.ind + 1], "-")? gzopen(argv[o.ind + 1], "r") : gzdopen(0, "r");
		assert(fp);
		ks = ks_init(fp);
		while (ks_getuntil(ks, KS_SEP_LINE, &str, 0) >= 0) {
			int32_t st1, en1, cov_st = 0, cov_en = 0;
			char *ctg, *rest;
			int64_t j;
			ctg = parse_bed3b(str.s, &st1, &en1, &rest);
			if (ctg == 0) continue;
			n_b = cr_overlap(cr, ctg, st1, en1, &b, &m_b);
			if (full) {
				if (n_b) {
					printf("%s\t%d\t%d", ctg, st1, en1);
					if (rest) puts(rest);
					else putchar('\n');
				}
			} else {
				for (j = 0; j < n_b; ++j) {
					cr_intv_t *r = &cr->r[b[j]];
					int32_t st0 = cr_st(r), en0 = cr_en(r);
					if (st0 < st1) st0 = st1;
					if (en0 > en1) en0 = en1;
					if (st0 > cov_en) {
						if (cov_en > cov_st) printf("%s\t%d\t%d\n", ctg, cov_st, cov_en);
						cov_st = st0, cov_en = en0;
					} else cov_en = cov_en > en0? cov_en : en0;
				}
				if (cov_en > cov_st) printf("%s\t%d\t%d\n", ctg, cov_st, cov_en);
			}
		}
		free(str.s);
		ks_destroy(ks);
		gzclose(fp);
	}

	free(b);
	cr_destroy(cr);
	return 0;
}

int main_cov(int argc, char *argv[])
{
	cgranges_t *cr;
	gzFile fp;
	ketopt_t o = KETOPT_INIT;
	kstream_t *ks;
	kstring_t str = {0,0,0};
	int64_t m_b = 0, *b = 0, n_b;
	int c, cnt_only = 0, contained = 0;

	while ((c = ketopt(&o, argc, argv, 1, "cC", 0)) >= 0) {
		if (c == 'c') cnt_only = 1;
		else if (c == 'C') contained = 1;
	}

	if (argc - o.ind < 1 || (argc - o.ind < 2 && isatty(0))) {
		printf("Usage: bedtk cov [options] <loaded.bed> <streamed.bed>\n");
		printf("Options:\n");
		printf("  -c       only count; no breadth of depth\n");
		printf("  -C       containment only\n");
		return 1;
	}

	cr = read_bed3(argv[o.ind]);
	assert(cr);
	cr_index(cr);

	fp = o.ind+1 < argc && strcmp(argv[o.ind + 1], "-")? gzopen(argv[o.ind + 1], "r") : gzdopen(0, "r");
	assert(fp);
	ks = ks_init(fp);
	while (ks_getuntil(ks, KS_SEP_LINE, &str, 0) >= 0) {
		int32_t st1, en1;
		char *ctg;
		int64_t j, cnt = 0, cov = 0, cov_st = 0, cov_en = 0;
		ctg = parse_bed3(str.s, &st1, &en1);
		if (ctg == 0) continue;
		if (contained)
			n_b = cr_contain(cr, ctg, st1, en1, &b, &m_b);
		else
			n_b = cr_overlap(cr, ctg, st1, en1, &b, &m_b);
		if (!cnt_only) {
			for (j = 0; j < n_b; ++j) {
				cr_intv_t *r = &cr->r[b[j]];
				int32_t st0 = cr_st(r), en0 = cr_en(r);
				if (st0 < st1) st0 = st1;
				if (en0 > en1) en0 = en1;
				if (st0 > cov_en) {
					cov += cov_en - cov_st;
					cov_st = st0, cov_en = en0;
				} else cov_en = cov_en > en0? cov_en : en0;
				++cnt;
			}
			cov += cov_en - cov_st;
			printf("%s\t%d\t%d\t%ld\t%ld\n", ctg, st1, en1, (long)cnt, (long)cov);
		} else printf("%s\t%d\t%d\t%ld\n", ctg, st1, en1, (long)n_b);
	}
	free(b);
	free(str.s);
	ks_destroy(ks);
	gzclose(fp);

	cr_destroy(cr);
	return 0;
}

int main_merge(int argc, char *argv[])
{
	cgranges_t *cr;
	ketopt_t o = KETOPT_INIT;
	int c, assume_srt = 0;

	while ((c = ketopt(&o, argc, argv, 1, "s", 0)) >= 0) {
		if (c == 's') assume_srt = 1;
	}

	if (argc - o.ind < 1 && isatty(0)) {
		printf("Usage: bedtk merge [options] <in.bed>\n");
		printf("Options:\n");
		printf("  -s       assume the input is sorted (NOT implemented yet)\n");
		return 1;
	}

	if (assume_srt) {
		fprintf(stderr, "ERROR: NOT implemented yet\n");
	} else {
		int64_t i;
		cr = read_bed3(o.ind < argc? argv[o.ind] : 0);
		assert(cr);
		if (!cr_is_sorted(cr)) cr_sort(cr);
		cr_merge_pre_index(cr);
		for (i = 0; i < cr->n_r; ++i) {
			const cr_intv_t *r = &cr->r[i];
			printf("%s\t%d\t%d\n", cr->ctg[r->x>>32].name, (int32_t)r->x, r->y);
		}
		cr_destroy(cr);
	}

	return 0;
}

int main_sum(int argc, char *argv[])
{
	cgranges_t *cr;
	ketopt_t o = KETOPT_INIT;
	int c, merge = 0;
	int64_t sum = 0;

	while ((c = ketopt(&o, argc, argv, 1, "m", 0)) >= 0) {
		if (c == 'm') merge = 1;
	}

	if (argc - o.ind < 1 && isatty(0)) {
		printf("Usage: bedtk sum [options] <in.bed>\n");
		printf("Options:\n");
		printf("  -m       merge overlapping regions\n");
		return 1;
	}

	if (!merge) {
		gzFile fp;
		kstream_t *ks;
		kstring_t str = {0,0,0};
		fp = o.ind < argc && strcmp(argv[o.ind], "-")? gzopen(argv[o.ind], "r") : gzdopen(0, "r");
		assert(fp);
		ks = ks_init(fp);
		while (ks_getuntil(ks, KS_SEP_LINE, &str, 0) >= 0) {
			int32_t st1, en1;
			char *ctg;
			ctg = parse_bed3(str.s, &st1, &en1);
			if (ctg == 0) continue;
			sum += en1 - st1;
		}
		free(str.s);
		ks_destroy(ks);
		gzclose(fp);
	} else {
		int64_t i;
		cr = read_bed3(o.ind < argc? argv[o.ind] : 0);
		assert(cr);
		if (!cr_is_sorted(cr)) cr_sort(cr);
		cr_merge_pre_index(cr);
		for (i = 0; i < cr->n_r; ++i)
			sum += cr->r[i].y - (int32_t)cr->r[i].x;
		cr_destroy(cr);
	}
	printf("%lld\n", (long long)sum);
	return 0;
}

int main_sort(int argc, char *argv[])
{
	cgranges_t *cr;
	ketopt_t o = KETOPT_INIT;
	int c;
	int64_t i;
	bed_rest_t rest;

	while ((c = ketopt(&o, argc, argv, 1, "", 0)) >= 0) {
	}

	if (argc - o.ind < 1 && isatty(0)) {
		printf("Usage: bedtk sort <in.bed>\n");
		return 1;
	}

	cr = read_bed3b(o.ind < argc? argv[o.ind] : 0, &rest);
	assert(cr);
	if (!cr_is_sorted(cr)) cr_sort(cr);
	for (i = 0; i < cr->n_r; ++i) {
		const cr_intv_t *r = &cr->r[i];
		printf("%s\t%d\t%d", cr->ctg[r->x>>32].name, (int32_t)r->x, r->y);
		if (rest.a[r->label].l) puts(rest.a[r->label].s);
		else putchar('\n');
	}
	cr_destroy(cr);

	for (i = 0; i < rest.n; ++i) free(rest.a[i].s);
	free(rest.a);
	return 0;
}

/*****************
 * Main function *
 *****************/

static int usage(FILE *fp)
{
	fprintf(fp, "Usage: bedtk <command> <arguments>\n");
	fprintf(fp, "Command:\n");
	fprintf(fp, "  isec      intersection (bedtools intersect)\n");
	fprintf(fp, "  cov       breadth of coverage (bedtools coverage)\n");
	fprintf(fp, "  merge     merge overlapping regions (bedtools merge)\n");
	fprintf(fp, "  sort      sort regions (bedtools sort)\n");
	fprintf(fp, "  sum       total region length\n");
	fprintf(fp, "  version   version number\n");
	return fp == stdout? 0 : 1;
}

int main(int argc, char *argv[])
{
	if (argc == 1) return usage(stdout);
	if (strcmp(argv[1], "isec") == 0) return main_isec(argc-1, argv+1);
	else if (strcmp(argv[1], "cov") == 0) return main_cov(argc-1, argv+1);
	else if (strcmp(argv[1], "merge") == 0) return main_merge(argc-1, argv+1);
	else if (strcmp(argv[1], "sum") == 0) return main_sum(argc-1, argv+1);
	else if (strcmp(argv[1], "sort") == 0) return main_sort(argc-1, argv+1);
	else if (strcmp(argv[1], "version") == 0) {
		puts(BEDTK_VERSION);
		return 0;
	} else {
		fprintf(stderr, "ERROR: unrecognized command '%s'. Abort!\n", argv[1]);
		return 1;
	}
}
