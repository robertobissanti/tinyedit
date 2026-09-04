#!/bin/sh
# Shell highlighting fixture
nome='tinyedit'
if [ -n "$nome" ]; then
    printf 'ciao %s\n' "$nome"
fi
