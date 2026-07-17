/* Curated target species list — see target_species.h. Order follows the
 * operator's Artsliste.txt (used verbatim as the relabel-picker order). */
#include "target_species.h"
#include <string.h>

const target_species_t TARGET_SPECIES[] = {
    { "Parus major",              "Great Tit" },                   /* Kjøttmeis */
    { "Cyanistes caeruleus",      "Eurasian Blue Tit" },           /* Blåmeis */
    { "Fringilla coelebs",        "Common Chaffinch" },            /* Bokfink */
    { "Pyrrhula pyrrhula",        "Eurasian Bullfinch" },          /* Dompap */
    { "Pica pica",                "Eurasian Magpie" },             /* Skjære */
    { "Emberiza citrinella",      "Yellowhammer" },                /* Gulspurv */
    { "Perisoreus infaustus",     "Siberian Jay" },                /* Lavskrike */
    { "Fringilla montifringilla", "Brambling" },                   /* Bjørkefink */
    { "Turdus iliacus",           "Redwing" },                     /* Rødvingetrost */
    { "Turdus pilaris",           "Fieldfare" },                   /* Gråtrost */
    { "Erithacus rubecula",       "European Robin" },              /* Rødstrupe */
    { "Lophophanes cristatus",    "Crested Tit" },                 /* Toppmeis */
    { "Periparus ater",           "Coal Tit" },                    /* Svartmeis */
    { "Anthus pratensis",         "Meadow Pipit" },                /* Heipiplerke */
    { "Oenanthe oenanthe",        "Northern Wheatear" },           /* Steinskvett */
    { "Pluvialis apricaria",      "European Golden Plover" },      /* Heilo */
    { "Eremophila alpestris",     "Horned Lark" },                 /* Fjellerke */
    { "Luscinia svecica",         "Bluethroat" },                  /* Blåstrupe */
    { "Corvus corax",             "Northern Raven" },              /* Ravn */
    { "Ficedula hypoleuca",       "European Pied Flycatcher" },    /* Svarthvit fluesnapper */
    { "Muscicapa striata",        "Spotted Flycatcher" },          /* Gråfluesnapper */
    { "Regulus regulus",          "Goldcrest" },                   /* Fuglekonge */
    { "Troglodytes troglodytes",  "Eurasian Wren" },               /* Gjerdesmett */
    { "Turdus merula",            "Common Blackbird" },            /* Svarttrost */
    { "Turdus philomelos",        "Song Thrush" },                 /* Måltrost */
    { "Dendrocopos major",        "Great Spotted Woodpecker" },    /* Flaggspett */
    { "Picoides tridactylus",     "Eurasian Three-toed Woodpecker" }, /* Tretåspett */
    { "Chloris chloris",          "European Greenfinch" },         /* Grønnfink */
    { "Acanthis flammea",         "Common Redpoll" },              /* Gråsisik */
    { "Corvus cornix",            "Hooded Crow" },                 /* Kråke */
    { "Garrulus glandarius",      "Eurasian Jay" },                /* Nøtteskrike */
};
const size_t TARGET_SPECIES_N = sizeof(TARGET_SPECIES) / sizeof(TARGET_SPECIES[0]);

bool species_in_target(const char *latin)
{
    if (!latin || !latin[0]) return false;
    for (size_t i = 0; i < TARGET_SPECIES_N; i++)
        if (strcmp(TARGET_SPECIES[i].latin, latin) == 0) return true;
    return false;
}
