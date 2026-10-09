/*
 * Guest paths on a case-sensitive POSIX file system.
 *
 * Xbox file systems ignore case and titles rely on it: Def Jam opens
 * D:\GCONFIG.XML and D:\FONTS\FONTS.VIV, and its disc has gconfig.xml and
 * fonts/fonts.viv. Windows and a default macOS volume ignore case as well, so
 * a translator that passed the title's spelling through worked everywhere but
 * Linux, where every such open failed and the title went on with the data
 * missing. Pinned down here:
 *
 *   1. an existing file is found whatever case the title used, directories
 *      included;
 *   2. a file that does not exist yet keeps the title's spelling, inside the
 *      directory that does (so a save is created where the title will look);
 *   3. nothing above the mapped root is rewritten -- the player's folders are
 *      theirs, and two that differ only in case must stay distinct.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* The generated title provides these; a regression harness has no title, and
 * the kernel bridge comes along with the path code in the static archive. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

static void expect(const char *xbox, const char *want)
{
    char got[MAX_PATH];
    if (!xbox_translate_path(xbox, got, sizeof got)) {
        printf("FAIL: %s did not translate\n", xbox);
        failures++;
    } else if (strcmp(got, want) != 0) {
        printf("FAIL: %s\n  got  %s\n  want %s\n", xbox, got, want);
        failures++;
    }
}

int main(void)
{
    char root[] = "/tmp/kernel_path_posix.XXXXXX";
    char game[512], save[512], p[640], want[640];

    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    /* The root itself has mixed case, as a player's folder might, beside a
     * sibling that differs only in case. */
    snprintf(game, sizeof game, "%s/Game", root);
    snprintf(save, sizeof save, "%s/Save", root);
    mkdir(game, 0755);
    mkdir(save, 0755);
    snprintf(p, sizeof p, "%s/game", root);           mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/fonts", game);          mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/fonts/fonts.viv", game); touch(p);
    snprintf(p, sizeof p, "%s/gconfig.xml", game);    touch(p);
    snprintf(p, sizeof p, "%s/Assets", game);         mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Assets/Tint.XSH", game); touch(p);

    xbox_path_init(game, save);

    /* 1. Existing files and directories, whatever case was asked for. */
    snprintf(want, sizeof want, "%s/gconfig.xml", game);
    expect("D:\\GCONFIG.XML", want);
    snprintf(want, sizeof want, "%s/fonts/fonts.viv", game);
    expect("D:\\FONTS\\FONTS.VIV", want);
    expect("\\Device\\CdRom0\\Fonts\\fonts.VIV", want);
    snprintf(want, sizeof want, "%s/Assets/Tint.XSH", game);
    expect("D:\\assets\\tint.xsh", want);
    /* The exact spelling is still taken as it is. */
    expect("D:\\Assets\\Tint.XSH", want);

    /* 2. Not there yet: the directory is matched, the new name is kept. */
    snprintf(want, sizeof want, "%s/Assets/NewFile.dat", game);
    expect("D:\\ASSETS\\NewFile.dat", want);
    snprintf(want, sizeof want, "%s/missing/Deeper.txt", game);
    expect("D:\\missing\\Deeper.txt", want);

    /* 3. The mapped root is never rewritten: Game stays Game though a
     *    sibling 'game' exists, and the save root is used as configured. */
    snprintf(want, sizeof want, "%s/gconfig.xml", game);
    expect("d:\\gconfig.xml", want);
    snprintf(p, sizeof p, "%s/UserData/45410049", save);
    {
        char ud[640];
        snprintf(ud, sizeof ud, "%s/UserData", save);
        mkdir(ud, 0755);
        mkdir(p, 0755);
    }
    snprintf(want, sizeof want, "%s/UserData/45410049/ABC", save);
    expect("U:\\45410049\\ABC", want);

    {
        char cmd[700];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
        if (system(cmd) != 0)
            printf("note: could not remove %s\n", root);
    }
    if (failures) {
        printf("kernel_path_posix: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("kernel_path_posix: ALL PASS\n");
    return 0;
}
