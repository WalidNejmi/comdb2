ifeq ($(TESTSROOTDIR),)
  include ../testcase.mk
else
  include $(TESTSROOTDIR)/testcase.mk
endif
ifeq ($(TEST_TIMEOUT),)
	export TEST_TIMEOUT=5m
endif

tool:
	cmake --build ${BUILDDIR} --target txn_commit_flags_sanitizer sc_publication_fence_codec_sanitizer -j8