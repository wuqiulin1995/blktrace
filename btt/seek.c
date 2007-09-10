/*
 * blktrace output analysis: generate a timeline & gather statistics
 *
 * Copyright (C) 2006 Alan D. Brunelle <Alan.Brunelle@hp.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include "globals.h"

static struct file_info *seek_files = NULL;

struct seek_bkt {
	struct rb_node rb_node;
	long long sectors;
	int nseeks;
};

struct seeki {
	FILE *rfp, *wfp, *cfp;
	struct rb_root root;
	long long tot_seeks;
	double total_sectors;
	long long last_start, last_end;
};

static FILE *seek_open(char *str, char rw)
{
	FILE *fp;
	char *oname;

	if (seek_name == NULL) return NULL;

	oname = malloc(strlen(seek_name) + strlen(str) + 32);
	sprintf(oname, "%s_%s_%c.dat", seek_name, str, rw);
	if ((fp = fopen(oname, "w")) == NULL)
		perror(oname);
	else
		add_file(&seek_files, fp, oname);

	return fp;
}

static void __insert(struct rb_root *root, long long sectors)
{
	struct seek_bkt *sbp;
	struct rb_node *parent = NULL;
	struct rb_node **p = &root->rb_node;

	while (*p) {
		parent = *p;
		sbp = rb_entry(parent, struct seek_bkt, rb_node);
		if (sectors < sbp->sectors)
			p = &(*p)->rb_left;
		else if (sectors > sbp->sectors)
			p = &(*p)->rb_right;
		else {
			sbp->nseeks++;
			return;
		}
	}

	sbp = malloc(sizeof(struct seek_bkt));
	sbp->nseeks = 1;
	sbp->sectors = sectors;

	rb_link_node(&sbp->rb_node, parent, p);
	rb_insert_color(&sbp->rb_node, root);
}

static void __destroy(struct rb_node *n)
{
	if (n) {
		struct seek_bkt *sbp = rb_entry(n, struct seek_bkt, rb_node);

		__destroy(n->rb_left);
		__destroy(n->rb_right);
		free(sbp);
	}
}

void seek_clean(void)
{
	clean_files(&seek_files);
}

long long seek_dist(struct seeki *sip, struct io *iop)
{
	long long dist;
	long long start = BIT_START(iop), end = BIT_END(iop);

	if (seek_absolute)
		dist = start - sip->last_end;
	else {
		/* Some overlap means no seek */
		if (((sip->last_start <= start) && (start <= sip->last_end)) ||
		    ((sip->last_start <= end) && (end <= sip->last_end)))
			dist = 0;
		else if (start > sip->last_end)
			dist = start - sip->last_end;
		else
			dist = start - sip->last_start;

	}

	sip->last_start = start;
	sip->last_end = end;
	return dist;
}

void *seeki_init(char *str)
{
	struct seeki *sip = malloc(sizeof(struct seeki));

	sip->rfp = seek_open(str, 'r');
	sip->wfp = seek_open(str, 'w');
	sip->cfp = seek_open(str, 'c');
	sip->tot_seeks = 0;
	sip->total_sectors = 0.0;
	sip->last_start = sip->last_end = 0;
	memset(&sip->root, 0, sizeof(sip->root));

	return sip;
}

void seeki_exit(void *param)
{
	struct seeki *sip = param;

	/*
	 * Associated files are cleaned up by seek_clean
	 */
	__destroy(sip->root.rb_node);
	free(sip);
}

void seeki_add(void *handle, struct io *iop)
{
	struct seeki *sip = handle;
	char rw = IOP_READ(iop) ? 'r' : 'w';
	long long dist = seek_dist(sip, iop);
	double tstamp = BIT_TIME(iop->t.time);
	FILE *fp = IOP_READ(iop) ? sip->rfp : sip->wfp;

	if (fp)
		fprintf(fp, "%15.9lf %13lld %c\n", tstamp, dist, rw);
	if (sip->cfp)
		fprintf(sip->cfp, "%15.9lf %13lld %c\n", tstamp, dist, rw);

	dist = llabs(dist);
	sip->tot_seeks++;
	sip->total_sectors += dist;
	__insert(&sip->root, dist);
}

long long seeki_nseeks(void *handle)
{
	return ((struct seeki *)handle)->tot_seeks;
}

double seeki_mean(void *handle)
{
	struct seeki *sip = handle;
	return sip->total_sectors / sip->tot_seeks;
}

int __median(struct rb_node *n, long long sofar, long long target, long 
		   long *rvp)
{
	struct seek_bkt *sbp;

	sbp = rb_entry(n, struct seek_bkt, rb_node);
	if ((sofar + sbp->nseeks) >= target) {
		*rvp = sbp->sectors;
		return 1;
	}

	if (n->rb_left && __median(n->rb_left, sofar, target, rvp))
		return 1;

	if (n->rb_right && __median(n->rb_right, sofar, target, rvp))
		return 1;

	return 0;

}

long long seeki_median(void *handle)
{
	long long rval = 0LL;
	struct seeki *sip = handle;

	if (sip->root.rb_node)
		(void)__median(sip->root.rb_node, 0LL, sip->tot_seeks / 2, 
			       &rval);

	return rval;
}

void __mode(struct rb_node *n, struct mode *mp)
{
	struct seek_bkt *sbp;

	if (n->rb_left) __mode(n->rb_left, mp);
	if (n->rb_right) __mode(n->rb_right, mp);

	sbp = rb_entry(n, struct seek_bkt, rb_node);
	if (mp->modes == NULL) {
		mp->modes = malloc(sizeof(long long));
		mp->nmds = 0;
	}
	else if (sbp->nseeks > mp->most_seeks)
		mp->nmds = 0;
	else if (sbp->nseeks == mp->most_seeks)
		mp->modes = realloc(mp->modes, (mp->nmds + 1) * 
							sizeof(long long));
	else
		return;

	mp->most_seeks = sbp->nseeks;
	mp->modes[mp->nmds++] = sbp->sectors;
}

int seeki_mode(void *handle, struct mode *mp)
{
	struct seeki *sip = handle;
	struct rb_root *root = &sip->root;

	memset(mp, 0, sizeof(struct mode));
	if (root->rb_node) 
		__mode(root->rb_node, mp);

	return mp->nmds;
}
