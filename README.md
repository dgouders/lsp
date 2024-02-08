```
lsp(1)                           User commands                          lsp(1)

Name
       lsp - list pages (or least significant pager)

Synopsis
       lsp [option ...] [file ...]
       lsp -h
       lsp -v

Description
       lsp is a terminal pager that assists in paging through data, usually
       text — no more(1), no less(1).

       The given files are opened if file names are given as options.  Other‐
       wise lsp assumes input from stdin and tries to read from there.

       In addition to its ability to aid in paging through text files lsp has
       some knowledge about manual pages and offers help in viewing them:

       • Visit references
         Manual  pages  usually  refer to other manual pages and lsp allows to
         navigate those references and to visit them as  new  files  with  the
         ability  to  also  navigate  through all opened manual pages or other
         files.

         Here, lsp tries to minimize frustration caused by unavailable  refer‐
         ences and verifies their existance before offering them as references
         that can be visited.

       • Complete resizes
         In  windowing environments lsp does complete resizes when windows get
         resized.  This means it also reloads the manual page to fit  the  new
         window dimensions.

       • Apropos pseudo-file
         Search  for  manual pages using apropos(1); in the current most basic
         form it lists all known manual pages ready for text  search  and  for
         visiting any of those manual pages.

       • TOC mode
         lsp has an experimental TOC mode.

         This  is  a  three-level folding mode trying to list only section and
         sub-section names for quick navigation in manual pages.

         The TOC is created using naive heuristics which works  well  to  some
         extend, but it might be incomplete.  Users should keep that in mind.

Searching
       lsp   supports   searches   using  extended  regular  expressions  (see
       regex(7)).

       •      Initially, searches in both directions start at the top left  of
              the current page.

       •      If  the  search  pattern  is changed or a search continued after
              other movement or a change between searches and navigating  ref‐
              erences  happens with the last current match on the current page
              the new search starts from that position.

       •      Found matches are highlighted and the  first  one  (the  current
              match)  is  marked by a blinking curser at the end of the match.
              'n' and 'p' navigate to the individual matches forth  and  back,
              respectively.

       •      If  a  search  is  started lsp tries to place the first matching
              line on the middle of the screen so that context in both  direc‐
              tions is visible.

              Further  matches  on  the same page don’t cause further movement
              until the first/last line is reached.

       •      CTRL-l can be used to bring the line with the current  match  to
              the top of the screen.

       •      Highlighting  of  matches  remains  active when changing back to
              normal file movement.  Press ESC to  turn  off  highlighting  of
              matches.

Special Input Characters / Sequences
       lsp tries to hand over data as is to ncurses(3) but some data is either
       converted to text attributes or translated to some other data.

       This  happens  on  all input — lsp doesn’t distinguish between text and
       non-text input, it relies on a decent preprocessor.

       • SGR sequences
         SGR sequences are recognized and translated.

         Currently, only a limited subset is implemented mostly by looking  at
         what input git(1) feeds lsp with.

       • backspace sequences
         Those are sequences according to grotty(1) legacy output format, pro‐
         viding the text attributes bold, italics and bold italics.

       • carriage return ('\r')
         lsp  replaces carriage return with "ˆM", because the original has too
         toxic effects on lsp’s output.

         This can be changed with --keep-cr.

       • TAB ('\t')
         TAB stops are expanded to spaces according to their horizontal  posi‐
         tion x with a currently hardcoded maximum width of 8.

Toxic Manual Pages / lsp_cat
       Usually, lsp is able to detect manual pages and their names by inspect‐
       ing  the  first line of a file — cooperative manual pages provide their
       name in the header, twice.

       In real life, however, manual pages exist that don’t have  their  names
       in  the header but the name of their overall project name, for example.
       Docker is such an example that made lsp fail here.

       lsp now uses the fact that at least man-db’s man(1) provides the  envi‐
       ronment variable MAN_PN containing the name of the manual page.

       This is nice for cases where lsp is called by man(1) but when lsp opens
       further  manual pages only the then used pager process knows about that
       environment variable but not lsp that only reads the data sent by  that
       forked process.

       lsp  now  uses  this  bad  trick  to come to know names of additionally
       opened manual pages:

       •      Provide a script lsp_cat that  injects  a  line  containing  the
              value of MAN_PN like this:

                     <lsp-man-pn>groff(1)</lsp-man-pn>

              prior to then executing cat(1) to output the actual input data.

       •      lsp then provides this script as PAGER when executing man(1).

       •      lsp then extracts the name out of the first line of the received
              data before storing the rest as the content of the actual manual
              page.

       Not very beautiful, perhaps, but it works.

       The drawback of this trick is that we have manual pages that are acces‐
       sible  by  several  names, the printf(3) family, for example.  With in‐
       specting the header line, lsp would maintain only one file,  regardless
       how  many of those names the user uses to visit this manual page.  With
       the described trick, thes manual pages  can  cause  as  many  different
       files as names for the manual page exist to lsp.

       So, better ideas are welcome.

Options
       All  options  can  be  given on the command line or via the environment
       variable LSP_OPTIONS.  The short version of toggles can also be used as
       commands, e.g. the user can type -i while paging through a file to tog‐
       gle case sensitivity for searches.

       -a, --load-apropos
              Create an apropos pseudo-file.

       -c, --chop-lines
              Toggle chopping of lines that do  not  fit  the  current  screen
              width.

       -h, --help
              Output help and exit.

       -i, --no-case
              Toggle case sensitivity in searches.

       -I, --man-case
              Turn on case sensitivity for names of manual pages.

              This  is  used  for example to verify references to other manual
              pages.

       --keep-cr
              Do not translate carriage return to "ˆM" on output.

       -l, --log-file
              Specify a path to where write debugging output.

              This needs to be a template according to  mkstemp(3):  a  string
              ending with six characters XXXXXX.

       -n, --line-numbers
              Toggle visible line numbers.

       --no-color
              Disable colored output.

       -o, --output-file
              Specify output file to duplicate all read input.

       --reload-command
              Specify command to (re)load manual pages.

              The given string must contain exactly one %n and one %s.

              %n  is  a  placeholder for the name of the manual page and %s is
              one for the section.

              Default is "man %n.%s".

       -s, --search-string
              Specify an initial search string.

              lsp then starts with searching for that string and positions  to
              the first match or displays an error message.

       -V, --no-verify
              Toggle verification of references.

              Verification  of  references is an expensive procedure.  On slow
              machines users might want options in that case: this one can  be
              used  to  completely  turn  verification off.  This comes at the
              cost that unusable references might be presented.

              By default verification is on.

       -v, --version
              Output version information of lsp and exit.

       --verify-command
              Specify command to verify the existance of references.

              The given string must contain exactly one %n and one %s.

              %n is a placeholder for the name of the manual page  and  %s  is
              one for the section.

              Default is "man -w %n.%s > /dev/null 2>&1"

       --verify-with-apropos
              Use  the  entries  of  the pseudo-file Apropos for validation of
              references.

              This option can speed up  verification  of  references  signifi‐
              cantly but users should keep in mind that verification will then
              be as reliable as the system’s manual page index is.

              With  this option, the first usage of TAB or Shift-TAB will load
              the pseudo-file Apropos and create valid references for each  of
              its  entries;  all following reference actions will then be much
              faster (approx. O(m) with m being the length of the reference).

Commands
       < / >
              Move to first / last page respectively.

       Pg-Down / Pg-Up
              Forward/backward one page, respectively.

       Key-Down / Key-Up / Mouse-Wheel down / up
              Forward / backward one line, respectively.

       CTRL-l
              In search mode: bring current match to top of the page.

       ESC
              Turn off current highlighting of matches.

       TAB / S-TAB
              Navigate to next/previous reference respectively.

       SPACE
              Forward one page in file.

       ENTER
              Depends on the active mode:

              • In normal mode:
                     Forward one line in file.

              • If previous command was TAB or S-TAB:
                     Open reference at point, i.e. call ‘man <reference>’.

              • In TOC-mode:
                     Go to currently selected position in file.

       /
              Start a forward search for regular expression.

       ?
              Start a backward search for regular expression.

       B
              Change buffer; choose from list.

       a
              Create a pseudo-file with the output of apropos(1).

              That pseudo-file contains  short  descriptions  for  all  manual
              pages  known  to the system; those manual pages can also be vis‐
              ited with TAB / S-TAB and ENTER commands.

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
              Depends on the active mode:

              • In normal mode:
                  exit lsp.

              • In TOC-mode:
                  switch back to normal view.

              • In help-mode:
                  close help file.

              • In file selection:
                  exit selection without selecting a file; stay at the  former
                  one.

Environment
       LSP_OPTIONS
              All  command line options can also be specified using this vari‐
              able.

       LSP_OPEN / LESSOPEN
              Analogical to less(1), lsp supports an  input  preprocessor  but
              currently just the two basic forms:

              1)     A string with the command to invoke the preprocessor con‐
                     taining exactly one occurence of "%s" to be replaced with
                     the file name.

                     This  command  must  write  a filename to standard output
                     that lsp can use to read the data  it  should  offer  for
                     paging.

              2)     Same as 1) but starting with a pipe symbol "|" to form an
                     input pipe.

                     The  specified  command  must write to standard output to
                     hand over the data for paging to lsp.

       MAN_PN
              lsp expects man(1) to provide MAN_PN with the name of the manual
              page at hand.

See Also
       apropos(1), less(1), man(1), mandb(8), mkstemp(3), more(1), pg(1)

Bugs
       Report bugs at https://github.com/dgouders/lsp

0.5.0-rc1                         02/07/2024                            lsp(1)
```
