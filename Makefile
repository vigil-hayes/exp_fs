#
# Makefile for the linux ramfs routines.
#

obj-y += expfs.o

file-mmu-y := file-nommu.o
file-mmu-$(CONFIG_MMU) := file-mmu.o
expfs-objs += inode.o $(file-mmu-y)
