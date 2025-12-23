# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2023-2025 Dirk Gouders
#
mandir = ~/.man/man1

.PHONY: install
install: lsp-help.1 lsp.1 lsp_cat.1
	install -d $(mandir)
	install $? $(mandir)

%.1: %.man ../.version
	./gen-th $*
	cat $*.man | soelim -r > $@

.PHONY: clean
clean:
	@rm -f *.th
	@rm -f *~
