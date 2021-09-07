/*
 * Copyright (C) 2013-2014 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <unistd.h>

#include "storage_source.h"
#include "testutils.h"
#include "vircommand.h"
#include "virerror.h"
#include "virfile.h"
#include "virlog.h"
#include "virstoragefile.h"
#include "virstring.h"

#include "storage/storage_driver.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("tests.storagetest");

#define datadir abs_builddir "/virstoragedata"

/* This test creates the following files, all in datadir:

 * raw: 1024-byte raw file
 * qcow2: qcow2 file with 'raw' as backing
 * wrap: qcow2 file with 'qcow2' as backing
 * qed: qed file with 'raw' as backing
 * sub/link1: symlink to qcow2
 * sub/link2: symlink to wrap
 *
 * Relative names to these files are known at compile time, but absolute
 * names depend on where the test is run; for convenience,
 * we pre-populate the computation of these names for use during the test.
*/

static char *qemuimg;
static char *absraw;
static char *absqcow2;
static char *abswrap;
static char *absqed;
static char *absdir;
static char *abslink2;

static void
testCleanupImages(void)
{
    VIR_FREE(qemuimg);
    VIR_FREE(absraw);
    VIR_FREE(absqcow2);
    VIR_FREE(abswrap);
    VIR_FREE(absqed);
    VIR_FREE(absdir);
    VIR_FREE(abslink2);

    if (chdir(abs_builddir) < 0) {
        fprintf(stderr, "unable to return to correct directory, refusing to "
                "clean up %s\n", datadir);
        return;
    }

    virFileDeleteTree(datadir);
}


static virStorageSource *
testStorageFileGetMetadata(const char *path,
                           int format,
                           uid_t uid, gid_t gid)
{
    struct stat st;
    g_autoptr(virStorageSource) def = virStorageSourceNew();

    def->type = VIR_STORAGE_TYPE_FILE;
    def->format = format;

    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            def->type = VIR_STORAGE_TYPE_DIR;
        } else if (S_ISBLK(st.st_mode)) {
            def->type = VIR_STORAGE_TYPE_BLOCK;
        }
    }

    def->path = g_strdup(path);

    /* 20 is picked as an arbitrary depth, since the chains used here don't exceed it */
    if (virStorageSourceGetMetadata(def, uid, gid, 20, true) < 0)
        return NULL;

    return g_steal_pointer(&def);
}

static int
testPrepImages(void)
{
    int ret = EXIT_FAILURE;
    bool compat = false;
    g_autoptr(virCommand) cmd = NULL;
    g_autofree char *buf = NULL;

    qemuimg = virFindFileInPath("qemu-img");
    if (!qemuimg)
        goto skip;

    /* Clean up from any earlier failed tests */
    virFileDeleteTree(datadir);

    /* See if qemu-img supports '-o compat=xxx'.  If so, we force the
     * use of both v2 and v3 files; if not, it is v2 only but the test
     * still works. */
    cmd = virCommandNewArgList(qemuimg, "create", "-f", "qcow2",
                               "-o?", "/dev/null", NULL);
    virCommandSetOutputBuffer(cmd, &buf);
    if (virCommandRun(cmd, NULL) < 0)
        goto skip;
    if (strstr(buf, "compat "))
        compat = true;
    VIR_FREE(buf);

    absraw = g_strdup_printf("%s/raw", datadir);
    absqcow2 = g_strdup_printf("%s/qcow2", datadir);
    abswrap = g_strdup_printf("%s/wrap", datadir);
    absqed = g_strdup_printf("%s/qed", datadir);
    absdir = g_strdup_printf("%s/dir", datadir);
    abslink2 = g_strdup_printf("%s/sub/link2", datadir);

    if (g_mkdir_with_parents(datadir "/sub", 0777) < 0) {
        fprintf(stderr, "unable to create directory %s\n", datadir "/sub");
        goto cleanup;
    }
    if (g_mkdir_with_parents(datadir "/dir", 0777) < 0) {
        fprintf(stderr, "unable to create directory %s\n", datadir "/dir");
        goto cleanup;
    }

    if (chdir(datadir) < 0) {
        fprintf(stderr, "unable to test relative backing chains\n");
        goto cleanup;
    }

    buf = g_strdup_printf("%1024d", 0);
    if (virFileWriteStr("raw", buf, 0600) < 0) {
        fprintf(stderr, "unable to create raw file\n");
        goto cleanup;
    }

    /* Create a qcow2 wrapping relative raw; later on, we modify its
     * metadata to test other configurations */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "create", "-f", "qcow2", NULL);
    virCommandAddArgFormat(cmd, "-obacking_file=raw,backing_fmt=raw%s",
                           compat ? ",compat=0.10" : "");
    virCommandAddArg(cmd, "qcow2");
    if (virCommandRun(cmd, NULL) < 0)
        goto skip;
    /* Make sure our later uses of 'qemu-img rebase' will work */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "rebase", "-u", "-f", "qcow2",
                               "-F", "raw", "-b", "raw", "qcow2", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        goto skip;

    /* Create a second qcow2 wrapping the first, to be sure that we
     * can correctly avoid insecure probing.  */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "create", "-f", "qcow2", NULL);
    virCommandAddArgFormat(cmd, "-obacking_file=%s,backing_fmt=qcow2%s",
                           absqcow2, compat ? ",compat=1.1" : "");
    virCommandAddArg(cmd, "wrap");
    if (virCommandRun(cmd, NULL) < 0)
        goto skip;

    /* Create a qed file. */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "create", "-f", "qed", NULL);
    virCommandAddArgFormat(cmd, "-obacking_file=%s,backing_fmt=raw",
                           absraw);
    virCommandAddArg(cmd, "qed");
    if (virCommandRun(cmd, NULL) < 0)
        goto skip;

#ifdef WITH_SYMLINK
    /* Create some symlinks in a sub-directory. */
    if (symlink("../qcow2", datadir "/sub/link1") < 0 ||
        symlink("../wrap", datadir "/sub/link2") < 0) {
        fprintf(stderr, "unable to create symlink");
        goto cleanup;
    }
#endif

    ret = 0;
 cleanup:
    if (ret)
        testCleanupImages();
    return ret;

 skip:
    fputs("qemu-img is too old; skipping this test\n", stderr);
    ret = EXIT_AM_SKIP;
    goto cleanup;
}

enum {
    EXP_PASS = 0,
    EXP_FAIL = 1,
};

struct testChainData
{
    const char *testname;
    const char *start;
    virStorageFileFormat format;
    unsigned int flags;
};


static int
testStorageChain(const void *args)
{
    const struct testChainData *data = args;
    virStorageSource *elt;
    g_autoptr(virStorageSource) meta = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *actual = NULL;
    g_autofree char *expectpath = g_strdup_printf("%s/virstoragetestdata/out/%s",
                                                  abs_srcdir, data->testname);

    meta = testStorageFileGetMetadata(data->start, data->format, -1, -1);
    if (!meta) {
        if (data->flags & EXP_FAIL) {
            virResetLastError();
            return 0;
        }
        return -1;
    } else if (data->flags & EXP_FAIL) {
        fprintf(stderr, "call should have failed\n");
        return -1;
    }

    if (virGetLastErrorCode()) {
        fprintf(stderr, "call should not have reported error\n");
        return -1;
    }

    for (elt = meta; virStorageSourceIsBacking(elt); elt = elt->backingStore) {
        g_autofree char *strippedPath = virTestStablePath(elt->path);
        g_autofree char *strippedBackingStoreRaw = virTestStablePath(elt->backingStoreRaw);
        g_autofree char *strippedRelPath = virTestStablePath(elt->relPath);

        virBufferAsprintf(&buf,
                          "path:%s\n"
                          "backingStoreRaw: %s\n"
                          "capacity: %lld\n"
                          "encryption: %d\n"
                          "relPath:%s\n"
                          "type:%d\n"
                          "format:%d\n"
                          "protocol:%s\n"
                          "hostname:%s\n\n",
                          strippedPath,
                          strippedBackingStoreRaw,
                          elt->capacity,
                          !!elt->encryption,
                          strippedRelPath,
                          elt->type,
                          elt->format,
                          virStorageNetProtocolTypeToString(elt->protocol),
                          NULLSTR(elt->nhosts ? elt->hosts[0].name : NULL));
    }

    virBufferTrim(&buf, "\n");

    actual = virBufferContentAndReset(&buf);

    if (virTestCompareToFile(actual, expectpath) < 0)
        return -1;

    return 0;
}

struct testLookupData
{
    virStorageSource *chain;
    const char *target;
    virStorageSource *from;
    const char *name;
    unsigned int expIndex;
    virStorageSource *expMeta;
    virStorageSource *expParent;
};

static int
testStorageLookup(const void *args)
{
    const struct testLookupData *data = args;
    int ret = 0;
    virStorageSource *result;
    virStorageSource *actualParent;

    result = virStorageSourceChainLookup(data->chain, data->from,
                                         data->name, data->target, &actualParent);
    if (!data->expMeta)
        virResetLastError();

    if (data->expMeta != result) {
        fprintf(stderr, "meta: expected %s, got %s\n",
                NULLSTR(data->expMeta ? data->expMeta->path : NULL),
                NULLSTR(result ? result->path : NULL));
        ret = -1;
    }

    if (data->expIndex > 0) {
        if (!result) {
            fprintf(stderr, "index: resulting lookup is empty, can't match index\n");
            ret = -1;
        } else {
            if (result->id != data->expIndex) {
                fprintf(stderr, "index: expected %u, got %u\n", data->expIndex, result->id);
                ret = -1;
            }
        }
    }
    if (data->expParent != actualParent) {
        fprintf(stderr, "parent: expected %s, got %s\n",
                NULLSTR(data->expParent ? data->expParent->path : NULL),
                NULLSTR(actualParent ? actualParent->path : NULL));
        ret = -1;
    }

    return ret;
}


static virStorageSource backingchain[12];

static void
testPathRelativePrepare(void)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(backingchain); i++) {
        backingchain[i].type = VIR_STORAGE_TYPE_FILE;
        if (i < G_N_ELEMENTS(backingchain) - 1)
            backingchain[i].backingStore = &backingchain[i + 1];
        else
            backingchain[i].backingStore = NULL;

        backingchain[i].relPath = NULL;
    }

    /* normal relative backing chain */
    backingchain[0].path = (char *) "/path/to/some/img";

    backingchain[1].path = (char *) "/path/to/some/asdf";
    backingchain[1].relPath = (char *) "asdf";

    backingchain[2].path = (char *) "/path/to/some/test";
    backingchain[2].relPath = (char *) "test";

    backingchain[3].path = (char *) "/path/to/some/blah";
    backingchain[3].relPath = (char *) "blah";

    /* ovirt's backing chain */
    backingchain[4].path = (char *) "/path/to/volume/image1";

    backingchain[5].path = (char *) "/path/to/volume/image2";
    backingchain[5].relPath = (char *) "../volume/image2";

    backingchain[6].path = (char *) "/path/to/volume/image3";
    backingchain[6].relPath = (char *) "../volume/image3";

    backingchain[7].path = (char *) "/path/to/volume/image4";
    backingchain[7].relPath = (char *) "../volume/image4";

    /* some arbitrarily crazy backing chains */
    backingchain[8].path = (char *) "/crazy/base/image";

    backingchain[9].path = (char *) "/crazy/base/directory/stuff/volumes/garbage/image2";
    backingchain[9].relPath = (char *) "directory/stuff/volumes/garbage/image2";

    backingchain[10].path = (char *) "/crazy/base/directory/image3";
    backingchain[10].relPath = (char *) "../../../image3";

    backingchain[11].path = (char *) "/crazy/base/blah/image4";
    backingchain[11].relPath = (char *) "../blah/image4";
}


struct testPathRelativeBacking
{
    virStorageSource *top;
    virStorageSource *base;

    const char *expect;
};

static int
testPathRelative(const void *args)
{
    const struct testPathRelativeBacking *data = args;
    g_autofree char *actual = NULL;

    if (virStorageSourceGetRelativeBackingPath(data->top,
                                               data->base,
                                               &actual) < 0) {
        fprintf(stderr, "relative backing path resolution failed\n");
        return -1;
    }

    if (STRNEQ_NULLABLE(data->expect, actual)) {
        fprintf(stderr, "relative path resolution from '%s' to '%s': "
                "expected '%s', got '%s'\n",
                data->top->path, data->base->path,
                NULLSTR(data->expect), NULLSTR(actual));
        return -1;
    }

    return 0;
}


struct testBackingParseData {
    const char *backing;
    const char *expect;
    int rv;
};

static int
testBackingParse(const void *args)
{
    const struct testBackingParseData *data = args;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *xml = NULL;
    g_autoptr(virStorageSource) src = NULL;
    int rc;
    int erc = data->rv;
    unsigned int xmlformatflags = VIR_DOMAIN_DEF_FORMAT_SECURE;

    /* expect failure return code with NULL expected data */
    if (!data->expect)
        erc = -1;

    if ((rc = virStorageSourceNewFromBackingAbsolute(data->backing, &src)) != erc) {
        fprintf(stderr, "expected return value '%d' actual '%d'\n", erc, rc);
        return -1;
    }

    if (!src)
        return 0;

    if (src && !data->expect) {
        fprintf(stderr, "parsing of backing store string '%s' should "
                        "have failed\n", data->backing);
        return -1;
    }

    if (virDomainDiskSourceFormat(&buf, src, "source", 0, false, xmlformatflags,
                                  false, false, NULL) < 0 ||
        !(xml = virBufferContentAndReset(&buf))) {
        fprintf(stderr, "failed to format disk source xml\n");
        return -1;
    }

    if (STRNEQ(xml, data->expect)) {
        fprintf(stderr, "\n backing store string '%s'\n"
                        "expected storage source xml:\n%s\n"
                        "actual storage source xml:\n%s\n",
                        data->backing, data->expect, xml);
        return -1;
    }

    return 0;
}


static int
mymain(void)
{
    int ret;
    struct testChainData data;
    struct testLookupData data2;
    struct testPathRelativeBacking data4;
    struct testBackingParseData data5;
    virStorageSource fakeChain[4];
    virStorageSource *chain = &fakeChain[0];
    virStorageSource *chain2 = &fakeChain[1];
    virStorageSource *chain3 = &fakeChain[2];
    g_autoptr(virCommand) cmd = NULL;

    if (storageRegisterAll() < 0)
       return EXIT_FAILURE;

    /* Prep some files with qemu-img; if that is not found on PATH, or
     * if it lacks support for qcow2 and qed, skip this test.  */
    if ((ret = testPrepImages()) != 0)
        return ret;

#define TEST_CHAIN(testname, start, format, flags) \
    do { \
        data = (struct testChainData){ testname, start, format, flags }; \
        if (virTestRun(testname, testStorageChain, &data) < 0) \
            ret = -1; \
    } while (0)

    /* Missing file */
    TEST_CHAIN("missing", "bogus", VIR_STORAGE_FILE_RAW, EXP_FAIL);

    /* Raw image, whether with right format or no specified format */
    TEST_CHAIN("raw-raw", absraw, VIR_STORAGE_FILE_RAW, EXP_PASS);
    TEST_CHAIN("raw-auto", absraw, VIR_STORAGE_FILE_AUTO, EXP_PASS);

    /* Qcow2 file with relative raw backing, format provided */
    TEST_CHAIN("qcow2-qcow2_raw-raw-relative", absqcow2, VIR_STORAGE_FILE_QCOW2, EXP_PASS);
    TEST_CHAIN("qcow2-auto_raw-raw-relative", absqcow2, VIR_STORAGE_FILE_AUTO, EXP_PASS);

    /* Rewrite qcow2 file to use absolute backing name */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "rebase", "-u", "-f", "qcow2",
                               "-F", "raw", "-b", absraw, "qcow2", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        ret = -1;

    /* Qcow2 file with raw as absolute backing, backing format provided */
    TEST_CHAIN("qcow2-qcow2_raw-raw", absqcow2, VIR_STORAGE_FILE_QCOW2, EXP_PASS);
    TEST_CHAIN("qcow2-auto_raw-raw", absqcow2, VIR_STORAGE_FILE_AUTO, EXP_PASS);

    /* qcow2 with a longer backing chain */
    TEST_CHAIN("qcow2-qcow2_qcow2-qcow2_raw-raw", abswrap, VIR_STORAGE_FILE_QCOW2, EXP_PASS);

    /* Rewrite qcow2 to a missing backing file, with backing type */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "rebase", "-u", "-f", "qcow2",
                               "-F", "qcow2", "-b", datadir "/bogus",
                               "qcow2", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        ret = -1;

    /* Qcow2 file with missing backing file but specified type */
    TEST_CHAIN("qcow2-qcow2_missing", absqcow2, VIR_STORAGE_FILE_QCOW2, EXP_FAIL);


    /* Rewrite qcow2 to use an nbd: protocol as backend */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "rebase", "-u", "-f", "qcow2",
                               "-F", "raw", "-b", "nbd+tcp://example.org:6000/blah",
                               "qcow2", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        ret = -1;

    /* Qcow2 file with backing protocol instead of file */
    TEST_CHAIN("qcow2-qcow2_nbd-raw", absqcow2, VIR_STORAGE_FILE_QCOW2, EXP_PASS);

    /* qed file */
    TEST_CHAIN("qed-qed_raw", absqed, VIR_STORAGE_FILE_QED, EXP_PASS);
    TEST_CHAIN("qed-auto_raw", absqed, VIR_STORAGE_FILE_AUTO, EXP_PASS);

    /* directory */
    TEST_CHAIN("directory-raw", absdir, VIR_STORAGE_FILE_RAW, EXP_PASS);
    TEST_CHAIN("directory-none", absdir, VIR_STORAGE_FILE_NONE, EXP_PASS);
    TEST_CHAIN("directory-dir", absdir, VIR_STORAGE_FILE_DIR, EXP_PASS);

#ifdef WITH_SYMLINK
    /* Rewrite qcow2 and wrap file to use backing names relative to a
     * symlink from a different directory */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "rebase", "-u", "-f", "qcow2",
                               "-F", "raw", "-b", "../raw", "qcow2", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        ret = -1;

    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "rebase", "-u", "-f", "qcow2",
                               "-F", "qcow2", "-b", "../sub/link1", "wrap",
                               NULL);
    if (virCommandRun(cmd, NULL) < 0)
        ret = -1;

    /* Behavior of symlinks to qcow2 with relative backing files */
    TEST_CHAIN("qcow2-symlinks", abslink2, VIR_STORAGE_FILE_QCOW2, EXP_PASS);
#endif

    /* Rewrite qcow2 to be a self-referential loop */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "rebase", "-u", "-f", "qcow2",
                               "-F", "qcow2", "-b", "qcow2", "qcow2", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        ret = -1;

    /* Behavior of an infinite loop chain */
    TEST_CHAIN("qcow2-qcow2_infinite-self", absqcow2, VIR_STORAGE_FILE_QCOW2, EXP_FAIL);

    /* Rewrite wrap and qcow2 to be mutually-referential loop */
    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "rebase", "-u", "-f", "qcow2",
                               "-F", "qcow2", "-b", "wrap", "qcow2", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        ret = -1;

    virCommandFree(cmd);
    cmd = virCommandNewArgList(qemuimg, "rebase", "-u", "-f", "qcow2",
                               "-F", "qcow2", "-b", absqcow2, "wrap", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        ret = -1;

    /* Behavior of an infinite loop chain */
    TEST_CHAIN("qcow2-qcow2_infinite-mutual", abswrap, VIR_STORAGE_FILE_QCOW2, EXP_FAIL);

    /* setup data for backing chain lookup testing */
    if (chdir(abs_srcdir "/virstoragetestdata/lookup") < 0) {
        fprintf(stderr, "unable to test relative backing chains\n");
        goto cleanup;
    }

    memset(fakeChain, 0, sizeof(fakeChain));
    fakeChain[0].backingStore = &fakeChain[1];
    fakeChain[1].backingStore = &fakeChain[2];
    fakeChain[2].backingStore = &fakeChain[3];

    fakeChain[0].type = VIR_STORAGE_TYPE_FILE;
    fakeChain[1].type = VIR_STORAGE_TYPE_FILE;
    fakeChain[2].type = VIR_STORAGE_TYPE_FILE;

    fakeChain[0].format = VIR_STORAGE_FILE_QCOW2;
    fakeChain[1].format = VIR_STORAGE_FILE_QCOW2;
    fakeChain[2].format = VIR_STORAGE_FILE_RAW;

    /* backing chain with relative start and absolute backing paths */
    fakeChain[0].path = (char *) "wrap";
    fakeChain[1].path = (char *) abs_srcdir "/virstoragetestdata/lookup/qcow2";
    fakeChain[2].path = (char *) abs_srcdir "/virstoragetestdata/lookup/raw";

#define TEST_LOOKUP_TARGET(id, target, from, name, index, meta, parent) \
    do { \
        data2 = (struct testLookupData){ \
            chain, target, from, name, index, meta, parent, }; \
        if (virTestRun("Chain lookup " #id, testStorageLookup, &data2) < 0) \
            ret = -1; \
    } while (0)
#define TEST_LOOKUP(id, from, name, meta, parent) \
    TEST_LOOKUP_TARGET(id, NULL, from, name, 0, meta, parent)

    TEST_LOOKUP(0, NULL, "bogus", NULL, NULL);
    TEST_LOOKUP(1, chain, "bogus", NULL, NULL);
    TEST_LOOKUP(2, NULL, "wrap", chain, NULL);
    TEST_LOOKUP(3, chain, "wrap", NULL, NULL);
    TEST_LOOKUP(4, chain2, "wrap", NULL, NULL);
    TEST_LOOKUP(5, NULL, abs_srcdir "/virstoragetestdata/lookup/wrap", chain, NULL);
    TEST_LOOKUP(6, chain, abs_srcdir "/virstoragetestdata/lookup/wrap", NULL, NULL);
    TEST_LOOKUP(7, chain2, abs_srcdir "/virstoragetestdata/lookup/wrap", NULL, NULL);
    TEST_LOOKUP(8, NULL, "qcow2", chain2, chain);
    TEST_LOOKUP(9, chain, "qcow2",  chain2, chain);
    TEST_LOOKUP(10, chain2, "qcow2", NULL, NULL);
    TEST_LOOKUP(11, chain3, "qcow2", NULL, NULL);
    TEST_LOOKUP(12, NULL, abs_srcdir "/virstoragetestdata/lookup/qcow2", chain2, chain);
    TEST_LOOKUP(13, chain, abs_srcdir "/virstoragetestdata/lookup/qcow2", chain2, chain);
    TEST_LOOKUP(14, chain2, abs_srcdir "/virstoragetestdata/lookup/qcow2", NULL, NULL);
    TEST_LOOKUP(15, chain3, abs_srcdir "/virstoragetestdata/lookup/qcow2", NULL, NULL);
    TEST_LOOKUP(16, NULL, "raw", chain3, chain2);
    TEST_LOOKUP(17, chain, "raw", chain3, chain2);
    TEST_LOOKUP(18, chain2, "raw", chain3, chain2);
    TEST_LOOKUP(19, chain3, "raw", NULL, NULL);
    TEST_LOOKUP(20, NULL, abs_srcdir "/virstoragetestdata/lookup/raw", chain3, chain2);
    TEST_LOOKUP(21, chain, abs_srcdir "/virstoragetestdata/lookup/raw", chain3, chain2);
    TEST_LOOKUP(22, chain2, abs_srcdir "/virstoragetestdata/lookup/raw", chain3, chain2);
    TEST_LOOKUP(23, chain3, abs_srcdir "/virstoragetestdata/lookup/raw", NULL, NULL);
    TEST_LOOKUP(24, NULL, NULL, chain3, chain2);
    TEST_LOOKUP(25, chain, NULL, chain3, chain2);
    TEST_LOOKUP(26, chain2, NULL, chain3, chain2);
    TEST_LOOKUP(27, chain3, NULL, NULL, NULL);

    /* relative backing, absolute start */
    fakeChain[0].path = (char *) abs_srcdir "/virstoragetestdata/lookup/wrap";

    fakeChain[1].relPath = (char *) "qcow2";
    fakeChain[2].relPath = (char *) "raw";

    TEST_LOOKUP(28, NULL, "bogus", NULL, NULL);
    TEST_LOOKUP(29, chain, "bogus", NULL, NULL);
    TEST_LOOKUP(30, NULL, "wrap", chain, NULL);
    TEST_LOOKUP(31, chain, "wrap", NULL, NULL);
    TEST_LOOKUP(32, chain2, "wrap", NULL, NULL);
    TEST_LOOKUP(33, NULL, abs_srcdir "/virstoragetestdata/lookup/wrap", chain, NULL);
    TEST_LOOKUP(34, chain, abs_srcdir "/virstoragetestdata/lookup/wrap", NULL, NULL);
    TEST_LOOKUP(35, chain2, abs_srcdir "/virstoragetestdata/lookup/wrap", NULL, NULL);
    TEST_LOOKUP(36, NULL, "qcow2", chain2, chain);
    TEST_LOOKUP(37, chain, "qcow2", chain2, chain);
    TEST_LOOKUP(38, chain2, "qcow2", NULL, NULL);
    TEST_LOOKUP(39, chain3, "qcow2", NULL, NULL);
    TEST_LOOKUP(40, NULL, abs_srcdir "/virstoragetestdata/lookup/qcow2", chain2, chain);
    TEST_LOOKUP(41, chain, abs_srcdir "/virstoragetestdata/lookup/qcow2", chain2, chain);
    TEST_LOOKUP(42, chain2, abs_srcdir "/virstoragetestdata/lookup/qcow2", NULL, NULL);
    TEST_LOOKUP(43, chain3, abs_srcdir "/virstoragetestdata/lookup/qcow2", NULL, NULL);
    TEST_LOOKUP(44, NULL, "raw", chain3, chain2);
    TEST_LOOKUP(45, chain, "raw", chain3, chain2);
    TEST_LOOKUP(46, chain2, "raw", chain3, chain2);
    TEST_LOOKUP(47, chain3, "raw", NULL, NULL);
    TEST_LOOKUP(48, NULL, abs_srcdir "/virstoragetestdata/lookup/raw", chain3, chain2);
    TEST_LOOKUP(49, chain, abs_srcdir "/virstoragetestdata/lookup/raw", chain3, chain2);
    TEST_LOOKUP(50, chain2, abs_srcdir "/virstoragetestdata/lookup/raw", chain3, chain2);
    TEST_LOOKUP(51, chain3, abs_srcdir "/virstoragetestdata/lookup/raw", NULL, NULL);
    TEST_LOOKUP(52, NULL, NULL, chain3, chain2);
    TEST_LOOKUP(53, chain, NULL, chain3, chain2);
    TEST_LOOKUP(54, chain2, NULL, chain3, chain2);
    TEST_LOOKUP(55, chain3, NULL, NULL, NULL);

    /* Use link to wrap with cross-directory relative backing */
    fakeChain[0].path = (char *) abs_srcdir "/virstoragetestdata/lookup/sub/link2";

    fakeChain[1].relPath = (char *) "../qcow2";
    fakeChain[2].relPath = (char *) "raw";

    TEST_LOOKUP(56, NULL, "bogus", NULL, NULL);
    TEST_LOOKUP(57, NULL, "sub/link2", chain, NULL);
    TEST_LOOKUP(58, NULL, "wrap", chain, NULL);
    TEST_LOOKUP(59, NULL, abs_srcdir "/virstoragetestdata/lookup/wrap", chain, NULL);
    TEST_LOOKUP(60, NULL, "../qcow2", chain2, chain);
    TEST_LOOKUP(61, NULL, "qcow2", NULL, NULL);
    TEST_LOOKUP(62, NULL, abs_srcdir "/virstoragetestdata/lookup/qcow2", chain2, chain);
    TEST_LOOKUP(63, NULL, "raw", chain3, chain2);
    TEST_LOOKUP(64, NULL, abs_srcdir "/virstoragetestdata/lookup/raw", chain3, chain2);
    TEST_LOOKUP(65, NULL, NULL, chain3, chain2);

    /* index lookup */
    fakeChain[0].id = 0;
    fakeChain[1].id = 1;
    fakeChain[2].id = 2;

    TEST_LOOKUP_TARGET(66, "vda", NULL, "bogus[1]", 0, NULL, NULL);
    TEST_LOOKUP_TARGET(67, "vda", NULL, "vda[-1]", 0, NULL, NULL);
    TEST_LOOKUP_TARGET(68, "vda", NULL, "vda[1][1]", 0, NULL, NULL);
    TEST_LOOKUP_TARGET(69, "vda", NULL, "wrap", 0, chain, NULL);
    TEST_LOOKUP_TARGET(70, "vda", chain, "wrap", 0, NULL, NULL);
    TEST_LOOKUP_TARGET(71, "vda", chain2, "wrap", 0, NULL, NULL);
    TEST_LOOKUP_TARGET(72, "vda", NULL, "vda[0]", 0, NULL, NULL);
    TEST_LOOKUP_TARGET(73, "vda", NULL, "vda[1]", 1, chain2, chain);
    TEST_LOOKUP_TARGET(74, "vda", chain, "vda[1]", 1, chain2, chain);
    TEST_LOOKUP_TARGET(75, "vda", chain2, "vda[1]", 0, NULL, NULL);
    TEST_LOOKUP_TARGET(76, "vda", chain3, "vda[1]", 0, NULL, NULL);
    TEST_LOOKUP_TARGET(77, "vda", NULL, "vda[2]", 2, chain3, chain2);
    TEST_LOOKUP_TARGET(78, "vda", chain, "vda[2]", 2, chain3, chain2);
    TEST_LOOKUP_TARGET(79, "vda", chain2, "vda[2]", 2, chain3, chain2);
    TEST_LOOKUP_TARGET(80, "vda", chain3, "vda[2]", 0, NULL, NULL);
    TEST_LOOKUP_TARGET(81, "vda", NULL, "vda[3]", 0, NULL, NULL);

#define TEST_RELATIVE_BACKING(id, TOP, BASE, EXPECT) \
    do { \
        data4.top = &TOP; \
        data4.base = &BASE; \
        data4.expect = EXPECT; \
        if (virTestRun("Path relative resolve " #id, \
                       testPathRelative, &data4) < 0) \
            ret = -1; \
    } while (0)

    testPathRelativePrepare();

    /* few negative tests first */

    /* a non-relative image is in the backing chain span */
    TEST_RELATIVE_BACKING(1, backingchain[0], backingchain[1], NULL);
    TEST_RELATIVE_BACKING(2, backingchain[0], backingchain[2], NULL);
    TEST_RELATIVE_BACKING(3, backingchain[0], backingchain[3], NULL);
    TEST_RELATIVE_BACKING(4, backingchain[1], backingchain[5], NULL);

    /* image is not in chain (specified backwards) */
    TEST_RELATIVE_BACKING(5, backingchain[2], backingchain[1], NULL);

    /* positive tests */
    TEST_RELATIVE_BACKING(6, backingchain[1], backingchain[1], "asdf");
    TEST_RELATIVE_BACKING(7, backingchain[1], backingchain[2], "test");
    TEST_RELATIVE_BACKING(8, backingchain[1], backingchain[3], "blah");
    TEST_RELATIVE_BACKING(9, backingchain[2], backingchain[2], "test");
    TEST_RELATIVE_BACKING(10, backingchain[2], backingchain[3], "blah");
    TEST_RELATIVE_BACKING(11, backingchain[3], backingchain[3], "blah");

    /* oVirt spelling */
    TEST_RELATIVE_BACKING(12, backingchain[5], backingchain[5], "../volume/image2");
    TEST_RELATIVE_BACKING(13, backingchain[5], backingchain[6], "../volume/../volume/image3");
    TEST_RELATIVE_BACKING(14, backingchain[5], backingchain[7], "../volume/../volume/../volume/image4");
    TEST_RELATIVE_BACKING(15, backingchain[6], backingchain[6], "../volume/image3");
    TEST_RELATIVE_BACKING(16, backingchain[6], backingchain[7], "../volume/../volume/image4");
    TEST_RELATIVE_BACKING(17, backingchain[7], backingchain[7], "../volume/image4");

    /* crazy spellings */
    TEST_RELATIVE_BACKING(17, backingchain[9], backingchain[9], "directory/stuff/volumes/garbage/image2");
    TEST_RELATIVE_BACKING(18, backingchain[9], backingchain[10], "directory/stuff/volumes/garbage/../../../image3");
    TEST_RELATIVE_BACKING(19, backingchain[9], backingchain[11], "directory/stuff/volumes/garbage/../../../../blah/image4");
    TEST_RELATIVE_BACKING(20, backingchain[10], backingchain[10], "../../../image3");
    TEST_RELATIVE_BACKING(21, backingchain[10], backingchain[11], "../../../../blah/image4");
    TEST_RELATIVE_BACKING(22, backingchain[11], backingchain[11], "../blah/image4");


    virTestCounterReset("Backing store parse ");

#define TEST_BACKING_PARSE_FULL(bck, xml, rc) \
    do { \
        data5.backing = bck; \
        data5.expect = xml; \
        data5.rv = rc; \
        if (virTestRun(virTestCounterNext(), \
                       testBackingParse, &data5) < 0) \
            ret = -1; \
    } while (0)

#define TEST_BACKING_PARSE(bck, xml) \
    TEST_BACKING_PARSE_FULL(bck, xml, 0)

    TEST_BACKING_PARSE("path", "<source file='path'/>\n");
    TEST_BACKING_PARSE("fat:/somedir", "<source dir='/somedir'/>\n");
    TEST_BACKING_PARSE("://", NULL);
    TEST_BACKING_PARSE("http://example.com",
                       "<source protocol='http' name=''>\n"
                       "  <host name='example.com' port='80'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("http://example.com/",
                       "<source protocol='http' name=''>\n"
                       "  <host name='example.com' port='80'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("http://example.com/file",
                       "<source protocol='http' name='file'>\n"
                       "  <host name='example.com' port='80'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE_FULL("http://user:pass@example.com/file",
                            "<source protocol='http' name='file'>\n"
                            "  <host name='example.com' port='80'/>\n"
                            "</source>\n", 1);
    TEST_BACKING_PARSE("rbd:testshare:id=asdf:mon_host=example.com",
                       "<source protocol='rbd' name='testshare'>\n"
                       "  <host name='example.com'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd:example.org:6000:exportname=blah",
                       "<source protocol='nbd' name='blah'>\n"
                       "  <host name='example.org' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd:example.org:6000:exportname=:",
                       "<source protocol='nbd' name=':'>\n"
                       "  <host name='example.org' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd:example.org:6000:exportname=:test",
                       "<source protocol='nbd' name=':test'>\n"
                       "  <host name='example.org' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd:[::1]:6000:exportname=:test",
                       "<source protocol='nbd' name=':test'>\n"
                       "  <host name='::1' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd:127.0.0.1:6000:exportname=:test",
                       "<source protocol='nbd' name=':test'>\n"
                       "  <host name='127.0.0.1' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd:unix:/tmp/sock:exportname=/",
                       "<source protocol='nbd' name='/'>\n"
                       "  <host transport='unix' socket='/tmp/sock'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd:unix:/tmp/sock:",
                       "<source protocol='nbd'>\n"
                       "  <host transport='unix' socket='/tmp/sock:'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd:unix:/tmp/sock::exportname=:",
                       "<source protocol='nbd' name=':'>\n"
                       "  <host transport='unix' socket='/tmp/sock:'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd://example.org:1234",
                       "<source protocol='nbd'>\n"
                       "  <host name='example.org' port='1234'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd://example.org:1234/",
                       "<source protocol='nbd'>\n"
                       "  <host name='example.org' port='1234'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd://example.org:1234/exportname",
                       "<source protocol='nbd' name='exportname'>\n"
                       "  <host name='example.org' port='1234'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd+unix://?socket=/tmp/sock",
                       "<source protocol='nbd'>\n"
                       "  <host transport='unix' socket='/tmp/sock'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd+unix:///?socket=/tmp/sock",
                       "<source protocol='nbd'>\n"
                       "  <host transport='unix' socket='/tmp/sock'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd+unix:////?socket=/tmp/sock",
                       "<source protocol='nbd' name='/'>\n"
                       "  <host transport='unix' socket='/tmp/sock'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd+unix:///exp?socket=/tmp/sock",
                       "<source protocol='nbd' name='exp'>\n"
                       "  <host transport='unix' socket='/tmp/sock'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("nbd+unix:////exp?socket=/tmp/sock",
                       "<source protocol='nbd' name='/exp'>\n"
                       "  <host transport='unix' socket='/tmp/sock'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE_FULL("iscsi://testuser:testpass@example.org:1234/exportname",
                            "<source protocol='iscsi' name='exportname'>\n"
                            "  <host name='example.org' port='1234'/>\n"
                            "</source>\n", 1);

#ifdef WITH_YAJL
    TEST_BACKING_PARSE("json:", NULL);
    TEST_BACKING_PARSE("json:asdgsdfg", NULL);
    TEST_BACKING_PARSE("json:{}", NULL);
    TEST_BACKING_PARSE("json: { \"file.driver\":\"blah\"}", NULL);
    TEST_BACKING_PARSE("json:{\"file.driver\":\"file\"}", NULL);
    TEST_BACKING_PARSE("json:{\"file.driver\":\"file\", "
                             "\"file.filename\":\"/path/to/file\"}",
                       "<source file='/path/to/file'/>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"file\", "
                             "\"filename\":\"/path/to/file\"}", NULL);
    TEST_BACKING_PARSE("json:{\"file\" : { \"driver\":\"file\","
                                          "\"filename\":\"/path/to/file\""
                                        "}"
                            "}",
                       "<source file='/path/to/file'/>\n");
    TEST_BACKING_PARSE("json:{\"driver\":\"file\","
                             "\"filename\":\"/path/to/file\""
                            "}",
                       "<source file='/path/to/file'/>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"host_device\", "
                             "\"file.filename\":\"/path/to/dev\"}",
                       "<source dev='/path/to/dev'/>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"host_cdrom\", "
                             "\"file.filename\":\"/path/to/cdrom\"}",
                       "<source dev='/path/to/cdrom'/>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"http\", "
                             "\"file.url\":\"http://example.com/file\"}",
                       "<source protocol='http' name='file'>\n"
                       "  <host name='example.com' port='80'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{ \"driver\":\"http\","
                                        "\"url\":\"http://example.com/file\""
                                      "}"
                            "}",
                       "<source protocol='http' name='file'>\n"
                       "  <host name='example.com' port='80'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"ftp\", "
                             "\"file.url\":\"http://example.com/file\"}",
                       NULL);
    TEST_BACKING_PARSE("json:{\"file.driver\":\"gluster\", "
                             "\"file.filename\":\"gluster://example.com/vol/file\"}",
                       "<source protocol='gluster' name='vol/file'>\n"
                       "  <host name='example.com' port='24007'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"gluster\","
                                       "\"volume\":\"testvol\","
                                       "\"path\":\"img.qcow2\","
                                       "\"server\":[ { \"type\":\"tcp\","
                                                      "\"host\":\"example.com\","
                                                      "\"port\":\"1234\""
                                                    "},"
                                                    "{ \"type\":\"unix\","
                                                      "\"socket\":\"/path/socket\""
                                                    "},"
                                                    "{ \"type\":\"tcp\","
                                                      "\"host\":\"example.com\""
                                                    "}"
                                                  "]"
                                      "}"
                             "}",
                        "<source protocol='gluster' name='testvol/img.qcow2'>\n"
                        "  <host name='example.com' port='1234'/>\n"
                        "  <host transport='unix' socket='/path/socket'/>\n"
                        "  <host name='example.com' port='24007'/>\n"
                        "</source>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"gluster\","
                             "\"file.volume\":\"testvol\","
                             "\"file.path\":\"img.qcow2\","
                             "\"file.server\":[ { \"type\":\"tcp\","
                                                 "\"host\":\"example.com\","
                                                 "\"port\":\"1234\""
                                               "},"
                                               "{ \"type\":\"unix\","
                                                 "\"socket\":\"/path/socket\""
                                               "},"
                                               "{ \"type\":\"inet\","
                                                 "\"host\":\"example.com\""
                                               "}"
                                             "]"
                            "}",
                        "<source protocol='gluster' name='testvol/img.qcow2'>\n"
                        "  <host name='example.com' port='1234'/>\n"
                        "  <host transport='unix' socket='/path/socket'/>\n"
                        "  <host name='example.com' port='24007'/>\n"
                        "</source>\n");
    TEST_BACKING_PARSE("json:{\"driver\": \"raw\","
                             "\"file\": {\"server.0.host\": \"A.A.A.A\","
                                        "\"server.1.host\": \"B.B.B.B\","
                                        "\"server.2.host\": \"C.C.C.C\","
                                        "\"driver\": \"gluster\","
                                        "\"path\": \"raw\","
                                        "\"server.0.type\": \"tcp\","
                                        "\"server.1.type\": \"tcp\","
                                        "\"server.2.type\": \"tcp\","
                                        "\"server.0.port\": \"24007\","
                                        "\"server.1.port\": \"24007\","
                                        "\"server.2.port\": \"24007\","
                                        "\"volume\": \"vol1\"}}",
                       "<source protocol='gluster' name='vol1/raw'>\n"
                       "  <host name='A.A.A.A' port='24007'/>\n"
                       "  <host name='B.B.B.B' port='24007'/>\n"
                       "  <host name='C.C.C.C' port='24007'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"nbd\","
                                       "\"path\":\"/path/to/socket\""
                                      "}"
                            "}",
                       "<source protocol='nbd'>\n"
                       "  <host transport='unix' socket='/path/to/socket'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"driver\":\"nbd\","
                             "\"path\":\"/path/to/socket\""
                            "}",
                       "<source protocol='nbd'>\n"
                       "  <host transport='unix' socket='/path/to/socket'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"nbd\","
                             "\"file.path\":\"/path/to/socket\""
                            "}",
                       "<source protocol='nbd'>\n"
                       "  <host transport='unix' socket='/path/to/socket'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"nbd\","
                                       "\"export\":\"blah\","
                                       "\"host\":\"example.org\","
                                       "\"port\":\"6000\""
                                      "}"
                            "}",
                       "<source protocol='nbd' name='blah'>\n"
                       "  <host name='example.org' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"nbd\","
                             "\"file.export\":\"blah\","
                             "\"file.host\":\"example.org\","
                             "\"file.port\":\"6000\""
                            "}",
                       "<source protocol='nbd' name='blah'>\n"
                       "  <host name='example.org' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"nbd\","
                                       "\"export\":\"blah\","
                                       "\"server\": { \"type\":\"inet\","
                                                     "\"host\":\"example.org\","
                                                     "\"port\":\"6000\""
                                                   "}"
                                      "}"
                            "}",
                       "<source protocol='nbd' name='blah'>\n"
                       "  <host name='example.org' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"nbd\","
                                       "\"server\": { \"type\":\"unix\","
                                                     "\"path\":\"/path/socket\""
                                                   "}"
                                      "}"
                            "}",
                       "<source protocol='nbd'>\n"
                       "  <host transport='unix' socket='/path/socket'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"ssh\","
                                       "\"host\":\"example.org\","
                                       "\"port\":\"6000\","
                                       "\"path\":\"blah\","
                                       "\"user\":\"user\""
                                      "}"
                            "}",
                       "<source protocol='ssh' name='blah'>\n"
                       "  <host name='example.org' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"ssh\","
                             "\"file.host\":\"example.org\","
                             "\"file.port\":\"6000\","
                             "\"file.path\":\"blah\","
                             "\"file.user\":\"user\""
                            "}",
                       "<source protocol='ssh' name='blah'>\n"
                       "  <host name='example.org' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"ssh\","
                                       "\"path\":\"blah\","
                                       "\"server\":{ \"host\":\"example.org\","
                                                    "\"port\":\"6000\""
                                                  "},"
                                       "\"user\":\"user\""
                                      "}"
                            "}",
                       "<source protocol='ssh' name='blah'>\n"
                       "  <host name='example.org' port='6000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file.driver\":\"rbd\","
                             "\"file.filename\":\"rbd:testshare:id=asdf:mon_host=example.com\""
                            "}",
                       "<source protocol='rbd' name='testshare'>\n"
                       "  <host name='example.com'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"rbd\","
                                       "\"image\":\"test\","
                                       "\"pool\":\"libvirt\","
                                       "\"conf\":\"/path/to/conf\","
                                       "\"snapshot\":\"snapshotname\","
                                       "\"server\":[ {\"host\":\"example.com\","
                                                      "\"port\":\"1234\""
                                                    "},"
                                                    "{\"host\":\"example2.com\""
                                                    "}"
                                                  "]"
                                      "}"
                             "}",
                        "<source protocol='rbd' name='libvirt/test'>\n"
                        "  <host name='example.com' port='1234'/>\n"
                        "  <host name='example2.com'/>\n"
                        "  <snapshot name='snapshotname'/>\n"
                        "  <config file='/path/to/conf'/>\n"
                        "</source>\n");
    TEST_BACKING_PARSE("json:{ \"file\": { "
                                "\"driver\": \"raw\","
                                "\"file\": {"
                                    "\"driver\": \"file\","
                                    "\"filename\": \"/path/to/file\" } } }",
                       "<source file='/path/to/file'/>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"iscsi\","
                                       "\"transport\":\"tcp\","
                                       "\"portal\":\"test.org\","
                                       "\"target\":\"iqn.2016-12.com.virttest:emulated-iscsi-noauth.target\""
                                      "}"
                            "}",
                       "<source protocol='iscsi' name='iqn.2016-12.com.virttest:emulated-iscsi-noauth.target/0'>\n"
                       "  <host name='test.org' port='3260'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE_FULL("json:{\"file\":{\"driver\":\"iscsi\","
                                            "\"transport\":\"tcp\","
                                            "\"portal\":\"test.org\","
                                            "\"user\":\"testuser\","
                                            "\"target\":\"iqn.2016-12.com.virttest:emulated-iscsi-auth.target\""
                                            "}"
                            "}",
                       "<source protocol='iscsi' name='iqn.2016-12.com.virttest:emulated-iscsi-auth.target/0'>\n"
                       "  <host name='test.org' port='3260'/>\n"
                       "</source>\n", 1);
    TEST_BACKING_PARSE_FULL("json:{\"file\":{\"driver\":\"iscsi\","
                                            "\"transport\":\"tcp\","
                                            "\"portal\":\"test.org\","
                                            "\"password\":\"testpass\","
                                            "\"target\":\"iqn.2016-12.com.virttest:emulated-iscsi-auth.target\""
                                            "}"
                            "}",
                       "<source protocol='iscsi' name='iqn.2016-12.com.virttest:emulated-iscsi-auth.target/0'>\n"
                       "  <host name='test.org' port='3260'/>\n"
                       "</source>\n", 1);
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"iscsi\","
                                       "\"transport\":\"tcp\","
                                       "\"portal\":\"test.org:1234\","
                                       "\"target\":\"iqn.2016-12.com.virttest:emulated-iscsi-noauth.target\","
                                       "\"lun\":\"6\""
                                      "}"
                            "}",
                       "<source protocol='iscsi' name='iqn.2016-12.com.virttest:emulated-iscsi-noauth.target/6'>\n"
                       "  <host name='test.org' port='1234'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"iscsi\","
                                       "\"transport\":\"tcp\","
                                       "\"portal\":\"[2001::0]:1234\","
                                       "\"target\":\"iqn.2016-12.com.virttest:emulated-iscsi-noauth.target\","
                                       "\"lun\":6"
                                      "}"
                            "}",
                       "<source protocol='iscsi' name='iqn.2016-12.com.virttest:emulated-iscsi-noauth.target/6'>\n"
                       "  <host name='[2001::0]' port='1234'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"iscsi\","
                                       "\"transport\":\"tcp\","
                                       "\"portal\":\"[2001::0]\","
                                       "\"target\":\"iqn.2016-12.com.virttest:emulated-iscsi-noauth.target\","
                                       "\"lun\":6"
                                      "}"
                            "}",
                       "<source protocol='iscsi' name='iqn.2016-12.com.virttest:emulated-iscsi-noauth.target/6'>\n"
                       "  <host name='[2001::0]' port='3260'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"sheepdog\","
                                       "\"vdi\":\"test\","
                                       "\"server\":{ \"type\":\"inet\","
                                                    "\"host\":\"example.com\","
                                                    "\"port\":\"321\""
                                                  "}"
                                      "}"
                            "}",
                       "<source protocol='sheepdog' name='test'>\n"
                       "  <host name='example.com' port='321'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"driver\": \"raw\","
                             "\"file\": {\"server.host\": \"10.10.10.10\","
                                        "\"server.port\": \"7000\","
                                        "\"tag\": \"\","
                                        "\"driver\": \"sheepdog\","
                                        "\"server.type\": \"inet\","
                                        "\"vdi\": \"Alice\"}}",
                       "<source protocol='sheepdog' name='Alice'>\n"
                       "  <host name='10.10.10.10' port='7000'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"vxhs\","
                                       "\"vdisk-id\":\"c6718f6b-0401-441d-a8c3-1f0064d75ee0\","
                                       "\"server\": {  \"host\":\"example.com\","
                                                      "\"port\":\"9999\""
                                                   "}"
                                      "}"
                            "}",
                       "<source protocol='vxhs' name='c6718f6b-0401-441d-a8c3-1f0064d75ee0'>\n"
                       "  <host name='example.com' port='9999'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE("json:{\"file\":{\"driver\":\"nfs\","
                                   "\"user\":2,"
                                   "\"group\":9,"
                                   "\"path\":\"/foo/bar/baz\","
                                   "\"server\": {  \"host\":\"example.com\","
                                                  "\"type\":\"inet\""
                                               "}"
                                      "}"
                            "}",
                       "<source protocol='nfs' name='/foo/bar/baz'>\n"
                       "  <host name='example.com'/>\n"
                       "  <identity user='+2' group='+9'/>\n"
                       "</source>\n");
    TEST_BACKING_PARSE_FULL("json:{ \"driver\": \"raw\","
                                    "\"offset\": 10752,"
                                    "\"size\": 4063232,"
                                    "\"file\": { \"driver\": \"file\","
                                                "\"filename\": \"/tmp/testfle\""
                                              "}"
                                  "}",
                            "<source file='/tmp/testfle'>\n"
                            "  <slices>\n"
                            "    <slice type='storage' offset='10752' size='4063232'/>\n"
                            "  </slices>\n"
                            "</source>\n", 0);

    TEST_BACKING_PARSE_FULL("json:{ \"file.cookie\": \"vmware_soap_session=\\\"0c8db85112873a79b7ef74f294cb70ef7f\\\"\","
                                   "\"file.sslverify\": false,"
                                   "\"file.driver\": \"https\","
                                   "\"file.url\": \"https://host/folder/esx6.5-rhel7.7-x86%5f64/esx6.5-rhel7.7-x86%5f64-flat.vmdk?dcPath=data&dsName=esx6.5-matrix\","
                                   "\"file.timeout\": 2000"
                                 "}",
                           "<source protocol='https' name='folder/esx6.5-rhel7.7-x86_64/esx6.5-rhel7.7-x86_64-flat.vmdk' query='dcPath=data&amp;dsName=esx6.5-matrix'>\n"
                           "  <host name='host' port='443'/>\n"
                           "  <ssl verify='no'/>\n"
                           "  <cookies>\n"
                           "    <cookie name='vmware_soap_session'>&quot;0c8db85112873a79b7ef74f294cb70ef7f&quot;</cookie>\n"
                           "  </cookies>\n"
                           "  <timeout seconds='2000'/>\n"
                           "</source>\n", 0);

    TEST_BACKING_PARSE_FULL("json:{ \"file.cookie\": \"vmware_soap_session=\\\"0c8db85112873a79b7ef74f294cb70ef7f\\\"\","
                                   "\"file.sslverify\": \"off\","
                                   "\"file.driver\": \"https\","
                                   "\"file.url\": \"https://host/folder/esx6.5-rhel7.7-x86%5f64/esx6.5-rhel7.7-x86%5f64-flat.vmdk?dcPath=data&dsName=esx6.5-matrix\","
                                   "\"file.timeout\": 2000"
                                 "}",
                           "<source protocol='https' name='folder/esx6.5-rhel7.7-x86_64/esx6.5-rhel7.7-x86_64-flat.vmdk' query='dcPath=data&amp;dsName=esx6.5-matrix'>\n"
                           "  <host name='host' port='443'/>\n"
                           "  <ssl verify='no'/>\n"
                           "  <cookies>\n"
                           "    <cookie name='vmware_soap_session'>&quot;0c8db85112873a79b7ef74f294cb70ef7f&quot;</cookie>\n"
                           "  </cookies>\n"
                           "  <timeout seconds='2000'/>\n"
                           "</source>\n", 0);

    TEST_BACKING_PARSE("json:{\"file\":{\"driver\": \"nvme\","
                                       "\"device\": \"0000:01:00.0\","
                                       "\"namespace\": 1"
                                      "}"
                            "}",
                        "<source type='pci' namespace='1'>\n"
                        "  <address domain='0x0000' bus='0x01' slot='0x00' function='0x0'/>\n"
                        "</source>\n");

#endif /* WITH_YAJL */

 cleanup:
    /* Final cleanup */
    testCleanupImages();

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN(mymain)
