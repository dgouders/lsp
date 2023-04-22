```
LSP(1)                           User commands                          LSP(1)

NAME
       lsp - list pages (or least significant pager)

SYNOPSIS
       lsp [options] [file_name]...

       lsp -h

       lsp -v

DESCRIPTION
       lsp is a terminal pager that assists in paging through data, usually
       text — no more(1), no less(1).

       The given files are opened if file names are given as options.
       Otherwise lsp assumes input from stdin and tries to read from there.

       In addition to it’s ability to aid in paging through text files lsp has
       limited knowledge about manual pages and offers some help in viewing
       them:

       •   Manual pages usually refer to other manual pages and lsp allows to
           navigate those references and to visit them as new files with the
           ability to also navigate through all opened manual pages or other
           files.

           Here, lsp tries to minimize frustration caused by unavailable
           references and verifies their existance before offering them as
           references that can be visited.

       •   In windowing environments lsp does complete resizes when windows
           get resized. This means it also reloads the manual page to fit the
           new window size.

       •   Search for manual pages using apropos(1); in the current most basic
           form it lists all known manual pages ready for text search and
           visiting referenced manual pages.

       •   lsp has an experimental TOC mode.

           This is a three-level folding mode trying to list only section and
           sub-section names for quick navigation in manual pages.

           The TOC is created using naive heuristics which works well to some
           extend, but it might be incomplete. Users should keep that in mind.

OPTIONS
       All options can be given on the command line or via the environment
       variable LSP_OPTIONS. The short version of toggles can also be used as
       commands, e.g. you can input -i while paging through a file to toggle
       case sensitivity for searches.

       -a, --load-apropos
           Create an apropos pseudo-file.

       -c, --chop-lines
           Toggle chopping of lines that do not fit the current screen width.

       -h, --help
           Output help and exit.

       -i, --no-case
           Toggle case sensitivity in searches.

       -I, --man-case
           Turn on case sensitivity for names of manual pages.

           This is used for example to verify references to other manual
           pages.

       -l, --log-file
           Specify a path to where write debugging output.

           This needs to be a template according to mkstemp(3); a string
           ending with six characters XXXXXX.

       -n, --line-numbers
           Toggle visible line numbers.

       --no-color
           Disable colored output.

       --reload-command
           Specify command to (re)load manual pages.

           The given string must contain exactly one %n and one %s.

           %n is a placeholder for the name of the manual page and %s is one
           for the section.

           Default is "man %n.%s".

       -s, --search-string
           Specify an initial search string.

           lsp then starts with searching for that string and positions to the
           first match or displays an error message.

       -V, --no-verify
           Toggle verification of references.

           Verification of references is an expensive procedure. On slow
           machines users might want options in that case: this one can be
           used to completely turn verification off. This comes at the cost
           that unusable references might be presented.

           By default verification is on.

       -v, --version
           Output version information of lsp and exit.

       --verify-command
           Specify command to verify the existance of references.

           The given string must contain exactly one %n and one %s.

           %n is a placeholder for the name of the manual page and %s is one
           for the section.

           Default is "man -w \"%n.%s\" > /dev/null 2>&1".

       --verify-with-apropos
           Use the entries of the pseudo-file Apropos for validation of
           references.

           This option can speed up verification of references significantly
           but users should keep in mind that verification will then be as
           reliable as the system’s manual page index is.

           With this option, the first usage of TAB or Shift-TAB will load the
           pseudo-file Apropos and create valid references for each of it’s
           entries; all following reference actions will then be much faster
           (approx. O(1)).

COMMANDS
       Pg-Down / Pg-Up
           Forward/backward one page, respectively.

       Key-Down / Key-Up / Mouse-Wheel down/up
           Forward/backward one line, respectively.

       CTRL-l
           In search mode: bring current match to top of the page.

       ESC
           Turn off current highlighting of matches.

       TAB / S-TAB
           Navigate to next/previous reference respectively.

       ENTER

           •   If previous command was TAB or S-TAB:

               Open reference at point, i.e. call ‘man <reference>'.

           •   In TOC-mode:

               Go to currently selected position in file.

       /
           Start a forward search for regular expression.

       ?
           Start a backward search for regular expression.

       B
           Change buffer; choose from list.

       a
           Create a pseudo-file with the output of ‘apropos .'.

           That pseudo-file contains short descriptions for all manual pages
           known to the system; those manual pages can also be opened with TAB
           / S-TAB and ENTER commands.

       b
           Backward one page

       c
           Close file currently paged.

           Exits lsp if it was the only/last file being paged.

       f
           Forward one page

       h
           Show online help with command summary.

       m
           Open another manual page.

       n
           Find next match in search.

       p
           Find previous match in search.

       q

           •   Exit lsp.

           •   In TOC-mode: switch back to normal view.

           •   In help-mode: close help file.

           •   In file selection: exit selection without selecting a file;
               stay at the former one

ENVIRONMENT
       LSP_OPTIONS
           All command line options can also be specified using this variable.

       LSP_OPEN / LESSOPEN
           Analogical to less(1), lsp supports an input preprocessor but
           currently just the two basic forms:

           One that provides the path to a replacement file and the one that
           writes the content to be paged to a pipe.

SEE ALSO
       apropos(1), less(1), man(1), mandb(8), mkstemp(3), more(1), pg(1)

BUGS
       Report bugs at https://github.com/dgouders/lsp

0.2.0                             04/22/2023                            LSP(1)
```
