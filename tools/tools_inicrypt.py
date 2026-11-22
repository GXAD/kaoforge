#!/usr/bin/env python3
"""Encrypt/decrypt Kao2 ini files (symmetric XOR). Usage: tools_inicrypt.py in out"""
import sys
KEY = b"7ACAISPJAnzxjh8**(!@*)(_))_!C*H&U#J~~~~~~~((@())FFF#"
d = open(sys.argv[1], 'rb').read()
open(sys.argv[2], 'wb').write(bytes(b ^ KEY[i % len(KEY)] for i, b in enumerate(d)))
