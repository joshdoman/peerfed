.PHONY: FORCE
all: FORCE
	$(MAKE) -C .. peerfed_qt test_peerfed_qt
clean: FORCE
	$(MAKE) -C .. peerfed_qt_clean test_peerfed_qt_clean
check: FORCE
	$(MAKE) -C .. test_peerfed_qt_check
bitcoin-qt bitcoin-qt.exe: FORCE
	 $(MAKE) -C .. peerfed_qt
apk: FORCE
	$(MAKE) -C .. peerfed_qt_apk
