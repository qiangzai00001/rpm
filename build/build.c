/* RPM - Copyright (C) 1995 Red Hat Software
 * 
 * build.c - routines for preparing and building the sources
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <limits.h>

#include "build.h"
#include "header.h"
#include "spec.h"
#include "specP.h"
#include "rpmerr.h"
#include "rpmlib.h"
#include "messages.h"
#include "stringbuf.h"
#include "misc.h"
#include "pack.h"

struct Script {
    char *name;
    FILE *file;
};

struct Script *openScript(Spec spec, int builddir, char *name);
void writeScript(struct Script *script, char *s);
int execScript(struct Script *script);
void freeScript(struct Script *script);
int execPart(Spec s, char *sb, char *name, int builddir);
static int doSetupMacro(Spec spec, StringBuf sb, char *line);
static int doPatchMacro(Spec spec, StringBuf sb, char *line);
static char *do_untar(Spec spec, int c);
static char *do_patch(Spec spec, int c, int strip, char *dashb);
int isCompressed(char *file);
static void doSweep(Spec s);

static char build_subdir[1024];

struct Script *openScript(Spec spec, int builddir, char *name)
{
    struct Script *script = malloc(sizeof(struct Script));
    struct PackageRec *main_package = spec->packages;
    char *s;
    int_32 foo;

    if (! main_package) {
	error(RPMERR_INTERNAL, "Empty main package");
	exit(RPMERR_INTERNAL);
    }
    
    script->name = tempnam("/var/tmp", "rpmbuild");
    script->file = fopen(script->name, "w");

    /* Prepare the script */
    fprintf(script->file,
	    "#!/bin/sh -e\n"
	    "# Script generated by rpm\n\n");

    fprintf(script->file, "RPM_SOURCE_DIR=\"%s\"\n", getVar(RPMVAR_SOURCEDIR));
    fprintf(script->file, "RPM_BUILD_DIR=\"%s\"\n", getVar(RPMVAR_BUILDDIR));
    fprintf(script->file, "RPM_DOC_DIR=\"%s\"\n", getVar(RPMVAR_DOCDIR));
    fprintf(script->file, "RPM_OPT_FLAGS=\"%s\"\n", getVar(RPMVAR_OPTFLAGS));
    if (getVar(RPMVAR_ROOT)) {
	fprintf(script->file, "RPM_ROOT_DIR=\"%s\"\n", getVar(RPMVAR_ROOT));
    } else {
	fprintf(script->file, "RPM_ROOT_DIR=\"\"\n");
    }

    fprintf(script->file, "RPM_PACKAGE_NAME=\"%s\"\n", spec->name);
    getEntry(main_package->header, RPMTAG_VERSION, &foo, (void **)&s, &foo);
    fprintf(script->file, "RPM_PACKAGE_VERSION=\"%s\"\n", s);
    getEntry(main_package->header, RPMTAG_RELEASE, &foo, (void **)&s, &foo);
    fprintf(script->file, "RPM_PACKAGE_RELEASE=\"%s\"\n", s);

    if (isVerbose()) {
	fprintf(script->file, "set -x\n\n");
    } else {
	fprintf(script->file, "exec > /dev/null\n\n");
    }

    fprintf(script->file, "\necho Excuting: %s\n", name);
    fprintf(script->file, "cd %s\n\n", getVar(RPMVAR_BUILDDIR));
    if (builddir) {
	/* Need to cd to the actual build directory. */
	/* Note that this means we have to parse the */
	/* %prep section even if we aren't using it. */
	fprintf(script->file, "cd %s\n\n", build_subdir);
    }

    return script;
}

void writeScript(struct Script *script, char *s)
{
    fprintf(script->file, "%s", s);
}

int execScript(struct Script *script)
{
    int pid;
    int status;
    
    writeScript(script, "\nexit 0;\n");
    fclose(script->file);
    script->file = NULL;
    chmod(script->name, 0700);

    if (!(pid = fork())) {
	execl(script->name, script->name, NULL);
	error(RPMERR_SCRIPT, "Exec failed");
	exit(RPMERR_SCRIPT);
    }
    wait(&status);
    if (! WIFEXITED(status) || WEXITSTATUS(status)) {
	error(RPMERR_SCRIPT, "Bad exit status");
	exit(RPMERR_SCRIPT);
    }
    return 0;
}

void freeScript(struct Script *script)
{
    if (script->file)
	fclose(script->file);
    unlink(script->name);
    free(script->name);
    free(script);
}

int execPart(Spec s, char *sb, char *name, int builddir)
{
    struct Script *script;

    message(MESS_DEBUG, "RUNNING: %s\n", name);
    script = openScript(s, builddir, name);
    writeScript(script, sb);
    execScript(script);
    freeScript(script);
    return 0;
}

static void doSweep(Spec s)
{
    char buf[1024];

    if (strcmp(build_subdir, ".")) {
        struct Script *script;
        script = openScript(s, 0, "sweep");
        sprintf(buf, "rm -rf %s\n", build_subdir);
        writeScript(script, buf);
        execScript(script);
        freeScript(script);
    }
}

static int doSetupMacro(Spec spec, StringBuf sb, char *line)
{
    char *s, *s1, *version;
    int opt_a, opt_b, opt_c, opt_D, opt_T;
    char *opt_n;
    char buf[1024];

    opt_a = opt_b = -1;
    opt_c = opt_T = opt_D = 0;
    opt_n = NULL;
    
    strtok(line, " \t\n");  /* remove %setup */
    while ((s = strtok(NULL, " \t\n"))) {
	if (!strcmp(s, "-c")) {
	    opt_c = 1;
	} else if (!strcmp(s, "-T")) {
	    opt_T = 1;
	} else if (!strcmp(s, "-D")) {
	    opt_D = 1;
	} else if (!strcmp(s, "-n")) {
	    /* dir name */
	    opt_n = strtok(NULL, " \t\n");
	    if (! opt_n) {
		error(RPMERR_BADSPEC, "Need arg to %%setup -n");
		return(RPMERR_BADSPEC);
	    }
	} else if (!strcmp(s, "-a")) {
	    s = strtok(NULL, " \t\n");
	    if (! s) {
		error(RPMERR_BADSPEC, "Need arg to %%setup -a");
		return(RPMERR_BADSPEC);
	    }
	    s1 = NULL;
	    opt_a = strtoul(s, &s1, 10);
	    if ((*s1) || (s1 == s) || (opt_a == ULONG_MAX)) {
		error(RPMERR_BADSPEC, "Bad arg to %%setup -a: %s", s);
		return(RPMERR_BADSPEC);
	    }
	} else if (!strcmp(s, "-b")) {
	    s = strtok(NULL, " \t\n");
	    if (! s) {
		error(RPMERR_BADSPEC, "Need arg to %%setup -b");
		return(RPMERR_BADSPEC);
	    }
	    s1 = NULL;
	    opt_b = strtoul(s, &s1, 10);
	    if ((*s1) || (s1 == s) || (opt_b == ULONG_MAX)) {
		error(RPMERR_BADSPEC, "Bad arg to %%setup -b: %s", s);
		return(RPMERR_BADSPEC);
	    }
	} else {
	    error(RPMERR_BADSPEC, "Bad arg to %%setup: %s", s);
	    return(RPMERR_BADSPEC);
	}
    }

    /* All args parsed */
#if 0
    printf("a = %d\n", opt_a);
    printf("b = %d\n", opt_b);
    printf("c = %d\n", opt_c);
    printf("T = %d\n", opt_T);
    printf("D = %d\n", opt_D);
    printf("n = %s\n", opt_n);
#endif

    if (opt_n) {
	strcpy(build_subdir, opt_n);
    } else {
	strcpy(build_subdir, spec->name);
	strcat(build_subdir, "-");
	/* We should already have a version field */
	getEntry(spec->packages->header, RPMTAG_VERSION, NULL,
		 (void *) &version, NULL);
	strcat(build_subdir, version);
    }
    
    /* cd to the build dir */
    sprintf(buf, "cd %s", getVar(RPMVAR_BUILDDIR));
    appendLineStringBuf(sb, buf);
    
    /* delete any old sources */
    if (! opt_D) {
	sprintf(buf, "rm -rf %s", build_subdir);
	appendLineStringBuf(sb, buf);
    }

    /* if necessary, create and cd into the proper dir */
    if (opt_c) {
	sprintf(buf, "mkdir -p %s\ncd %s", build_subdir, build_subdir);
	appendLineStringBuf(sb, buf);
    }

    /* do the default action */
    if ((! opt_c) && (! opt_T)) {
	s = do_untar(spec, 0);
	if (! s) {
	    return 1;
	}
	appendLineStringBuf(sb, s);
    }

    /* do any before action */
    if (opt_b > -1) {
	s = do_untar(spec, opt_b);
	if (! s) {
	    return 1;
	}
	appendLineStringBuf(sb, s);
    }
    
    /* cd into the build subdir */
    if (!opt_c) {
	sprintf(buf, "cd %s", build_subdir);
	appendLineStringBuf(sb, buf);
    }

    if (opt_c && (! opt_T)) {
	s = do_untar(spec, 0);
	if (! s) {
	    return 1;
	}
	appendLineStringBuf(sb, s);
    }
    
    /* do any after action */
    if (opt_a > -1) {
	s = do_untar(spec, opt_a);
	if (! s) {
	    return 1;
	}
	appendLineStringBuf(sb, s);
    }

    /* clean up permissions etc */
    sprintf(buf, "cd %s/%s", getVar(RPMVAR_BUILDDIR), build_subdir);
    appendLineStringBuf(sb, buf);
    if (! geteuid()) {
	strcpy(buf, "chown -R root.root .");
	appendLineStringBuf(sb, buf);
    }
    strcpy(buf, "chmod -R a+rX,g-w,o-w .");
    appendLineStringBuf(sb, buf);
    
    return 0;
}

int isCompressed(char *file)
{
    int fd;
    unsigned char magic[4];

    fd = open(file, O_RDONLY);
    read(fd, magic, 4);
    close(fd);

    if (((magic[0] == 0037) && (magic[1] == 0213)) ||  /* gzip */
	((magic[0] == 0037) && (magic[1] == 0236)) ||  /* old gzip */
	((magic[0] == 0037) && (magic[1] == 0036)) ||  /* pack */
	((magic[0] == 0037) && (magic[1] == 0240)) ||  /* SCO lzh */
	((magic[0] == 0037) && (magic[1] == 0235)) ||  /* compress */
	((magic[0] == 0120) && (magic[1] == 0113) &&
	 (magic[2] == 0003) && (magic[3] == 0004))     /* pkzip */
	) {
	return 1;
    }

    return 0;
}

static char *do_untar(Spec spec, int c)
{
    static char buf[1024];
    char file[1024];
    char *s, *taropts;
    struct sources *sp;

    s = NULL;
    sp = spec->sources;
    while (sp) {
	if ((sp->ispatch == 0) && (sp->num == c)) {
	    s = sp->source;
	    break;
	}
	sp = sp->next;
    }
    if (! s) {
	error(RPMERR_BADSPEC, "No source number %d", c);
	return NULL;
    }

    sprintf(file, "%s/%s", getVar(RPMVAR_SOURCEDIR), s);
    taropts = (isVerbose() ? "-xvvf" : "-xf");
    
    if (isCompressed(file)) {
	sprintf(buf,
		"gzip -dc %s | tar %s -\n"
		"if [ $? -ne 0 ]; then\n"
		"  exit $?\n"
		"fi",
		file, taropts);
    } else {
	sprintf(buf, "tar %s %s", taropts, file);
    }

    return buf;
}

static char *do_patch(Spec spec, int c, int strip, char *db)
{
    static char buf[1024];
    char file[1024];
    char dashb[1024];
    char *s;
    struct sources *sp;

    s = NULL;
    sp = spec->sources;
    while (sp) {
	if ((sp->ispatch == 1) && (sp->num == c)) {
	    s = sp->source;
	    break;
	}
	sp = sp->next;
    }
    if (! s) {
	error(RPMERR_BADSPEC, "No patch number %d", c);
	return NULL;
    }

    sprintf(file, "%s/%s", getVar(RPMVAR_SOURCEDIR), s);

    if (db) {
	sprintf(dashb, "-b %s", db);
    } else {
	strcpy(dashb, "");
    }
    
    if (isCompressed(file)) {
	sprintf(buf,
		"gzip -dc %s | patch -p%d %s -s\n"
		"if [ $? -ne 0 ]; then\n"
		"  exit $?\n"
		"fi",
		file, strip, dashb);
    } else {
	sprintf(buf, "patch -p%d %s -s < %s", strip, dashb, file);
    }

    return buf;
}

static int doPatchMacro(Spec spec, StringBuf sb, char *line)
{
    char *opt_b;
    int opt_P, opt_p;
    char *s, *s1;
    char buf[1024];
    int patch_nums[1024];  /* XXX - we can only handle 1024 patches! */
    int patch_index, x;

    opt_P = opt_p = 0;
    opt_b = NULL;
    patch_index = 0;

    if (! index(" \t\n", line[6])) {
	/* %patchN */
	sprintf(buf, "%%patch -P %s", line + 6);
    } else {
	strcpy(buf, line);
    }
    
    strtok(buf, " \t\n");  /* remove %patch */
    while ((s = strtok(NULL, " \t\n"))) {
	if (!strcmp(s, "-P")) {
	    opt_P = 1;
	} else if (!strcmp(s, "-b")) {
	    /* orig suffix */
	    opt_b = strtok(NULL, " \t\n");
	    if (! opt_b) {
		error(RPMERR_BADSPEC, "Need arg to %%patch -b");
		return(RPMERR_BADSPEC);
	    }
	} else if (!strncmp(s, "-p", 2)) {
	    /* unfortunately, we must support -pX */
	    if (! index(" \t\n", s[2])) {
		s = s + 2;
	    } else {
		s = strtok(NULL, " \t\n");
		if (! s) {
		    error(RPMERR_BADSPEC, "Need arg to %%patch -p");
		    return(RPMERR_BADSPEC);
		}
	    }
	    s1 = NULL;
	    opt_p = strtoul(s, &s1, 10);
	    if ((*s1) || (s1 == s) || (opt_p == ULONG_MAX)) {
		error(RPMERR_BADSPEC, "Bad arg to %%patch -p: %s", s);
		return(RPMERR_BADSPEC);
	    }
	} else {
	    /* Must be a patch num */
	    if (patch_index == 1024) {
		error(RPMERR_BADSPEC, "Too many patches!");
		return(RPMERR_BADSPEC);
	    }
	    s1 = NULL;
	    patch_nums[patch_index] = strtoul(s, &s1, 10);
	    if ((*s1) || (s1 == s) || (patch_nums[patch_index] == ULONG_MAX)) {
		error(RPMERR_BADSPEC, "Bad arg to %%patch: %s", s);
		return(RPMERR_BADSPEC);
	    }
	    patch_index++;
	}
    }

    /* All args processed */

    if (! opt_P) {
	s = do_patch(spec, 0, opt_p, opt_b);
	if (! s) {
	    return 1;
	}
	appendLineStringBuf(sb, s);
    }

    x = 0;
    while (x < patch_index) {
	s = do_patch(spec, patch_nums[x], opt_p, opt_b);
	if (! s) {
	    return 1;
	}
	appendLineStringBuf(sb, s);
	x++;
    }
    
    return 0;
}

static int checkSources(Spec s)
{
    struct sources *source;
    struct PackageRec *package;
    char buf[1024];

    /* Check that we can access all the sources */
    source = s->sources;
    while (source) {
	sprintf(buf, "%s/%s", getVar(RPMVAR_SOURCEDIR), source->source);
	if (access(buf, R_OK)) {
	    error(RPMERR_BADSPEC, "missing source or patch: %s", buf);
	    return RPMERR_BADSPEC;
	}
	source = source->next;
    }

    /* ... and icons */
    package = s->packages;
    while (package) {
	if (package->icon) {
	    sprintf(buf, "%s/%s", getVar(RPMVAR_SOURCEDIR), package->icon);
	    if (access(buf, R_OK)) {
		error(RPMERR_BADSPEC, "missing icon: %s", buf);
		return RPMERR_BADSPEC;
	    }
	}
	package = package->next;
    }
    
    return 0;
}

int execPrep(Spec s, int really_exec)
{
    char **lines, **lines1, *p;
    StringBuf out;
    int res;

    if (checkSources(s)) {
	return 1;
    }
    out = newStringBuf();
    
    p = getStringBuf(s->prep);
    lines = splitString(p, strlen(p), '\n');
    lines1 = lines;
    while (*lines) {
	if (! strncasecmp(*lines, "%setup", 6)) {
	    if (doSetupMacro(s, out, *lines)) {
		return 1;
	    }
	} else if (! strncasecmp(*lines, "%patch", 6)) {
	    if (doPatchMacro(s, out, *lines)) {
		return 1;
	    }
	} else {
	    appendLineStringBuf(out, *lines);
	}
	lines++;
    }

    freeSplitString(lines1);
    res = 0;
    if (really_exec) {
	res = execPart(s, getStringBuf(out), "%prep", 0);
    }
    freeStringBuf(out);
    return res;
}

int execBuild(Spec s)
{
    return execPart(s, getStringBuf(s->build), "%build", 1);
}

int execInstall(Spec s)
{
    int res;

    if ((res = execPart(s, getStringBuf(s->install), "%install", 1))) {
	return res;
    }
    return execPart(s, getStringBuf(s->doc), "special doc", 1);
}

int execClean(Spec s)
{
    return execPart(s, getStringBuf(s->clean), "%clean", 1);
}

int verifyList(Spec s)
{
    return 0;
}

int doBuild(Spec s, int flags, char *passPhrase)
{

    strcpy(build_subdir, ".");

    if (flags & RPMBUILD_LIST) {
	if (verifyList(s)) {
	    return 1;
	}
    }

    /* We always need to parse the %prep section */
    if (execPrep(s, (flags & RPMBUILD_PREP))) {
	return 1;
    }

    if (flags & RPMBUILD_BUILD) {
	if (execBuild(s)) {
	    return 1;
	}
    }

    if (flags & RPMBUILD_INSTALL) {
	if (execInstall(s)) {
	    return 1;
	}
    }

    markBuildTime();
    
    if (flags & RPMBUILD_BINARY) {
	if (packageBinaries(s, passPhrase)) {
	    return 1;
	}
	if (execClean(s)) {
	    return 1;
	}
    }

    if (flags & RPMBUILD_SOURCE) {
	if (packageSource(s, passPhrase)) {
	    return 1;
	}
    }

    if (flags & RPMBUILD_SWEEP) {
	doSweep(s);
    }

    if (flags & RPMBUILD_RMSOURCE) {
	doRmSource(s);
    }

    return 0;
}
