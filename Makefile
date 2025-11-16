CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -lrt

TARGET = trimui_inputd_smart_pro

SRCDIR = src
BUILDDIR = build
OBJDIR = $(BUILDDIR)/obj
BINDIR = $(BUILDDIR)/$(TARGET)/bin

SRCS = SRCS = $(shell find $(SRCDIR) -type f -name "*.c")

OBJS = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

$(BINDIR)/$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

.PHONY: clean
clean:
	rm -rf $(BUILDDIR)

MKDIR_P ?= mkdir -p
