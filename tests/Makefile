# Tests sur PC des briques portables et de la logique des nœuds.
#   make        compile et lance tous les tests
CFLAGS = -Wall -Wextra -std=c11 -g -I../node/include -I..
SRC = ../node/src/crc16.c ../node/src/frame.c ../node/src/crypto.c
OUT = build

all: $(OUT)/test_briques $(OUT)/test_chaine
	./$(OUT)/test_briques
	./$(OUT)/test_chaine
	python3 -I test_outil_pc.py

$(OUT)/test_briques: test_briques.c $(SRC) | $(OUT)
	$(CC) $(CFLAGS) $^ -o $@

$(OUT)/test_chaine: test_chaine.c ../node/src/noeud.c $(SRC) | $(OUT)
	$(CC) $(CFLAGS) $^ -o $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
