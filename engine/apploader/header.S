/* Apploader header (disc offset 0x2440): build date, entry point, code size, trailer size. */
	.section .header, "a"
	.ascii "2026/11/07"
	.space 6
	.long apploader_entry
	.long _code_size
	.long 0
	.long 0
