/* Species display-name localization (FSD §3.2). The classifier's labels
 * are "Scientific name (English common name)"; this table adds Norwegian
 * common names for the species realistically seen at a Northern-European
 * nest box or feeder, covering the region the v1 global model was chosen
 * to serve (see docs/MODEL.md). Species outside the table still get a
 * correct result — the English name and the Latin binomial, which is
 * shown regardless of language and never needs translating. */
#include "species_i18n.h"
#include <string.h>
#include <stdio.h>

typedef struct { const char *latin; const char *no; } no_name_t;

static const no_name_t NO_NAMES[] = {
    { "Erithacus rubecula",     "Rødstrupe" },
    { "Parus major",            "Kjøttmeis" },
    { "Cyanistes caeruleus",    "Blåmeis" },
    { "Periparus ater",         "Svartmeis" },
    { "Aegithalos caudatus",    "Stjertmeis" },
    { "Sitta europaea",         "Spettmeis" },
    { "Troglodytes troglodytes","Gjerdesmett" },
    { "Prunella modularis",     "Jernspurv" },
    { "Turdus merula",          "Svarttrost" },
    { "Turdus philomelos",      "Måltrost" },
    { "Turdus pilaris",         "Gråtrost" },
    { "Turdus viscivorus",      "Duetrost" },
    { "Passer domesticus",      "Gråspurv" },
    { "Passer montanus",        "Pilfink" },
    { "Fringilla coelebs",      "Bokfink" },
    { "Fringilla montifringilla","Bjørkefink" },
    { "Chloris chloris",        "Grønnfink" },
    { "Carduelis carduelis",    "Stillits" },
    { "Carduelis cannabina",    "Tornirisk" },
    { "Spinus spinus",          "Grønnsisik" },
    { "Pyrrhula pyrrhula",      "Dompap" },
    { "Sturnus vulgaris",       "Stær" },
    { "Corvus corone",          "Svartkråke" },
    { "Corvus cornix",          "Kråke" },
    { "Corvus monedula",        "Kaie" },
    { "Corvus frugilegus",      "Kornkråke" },
    { "Corvus corax",           "Ravn" },
    { "Pica pica",              "Skjære" },
    { "Garrulus glandarius",    "Nøtteskrike" },
    { "Columba palumbus",       "Ringdue" },
    { "Columba livia",          "Klippedue" },
    { "Columba livia domestica","Bydue" },
    { "Streptopelia decaocto",  "Tyrkerdue" },
    { "Streptopelia turtur",    "Turteldue" },
    { "Motacilla alba",         "Linerle" },
    { "Motacilla cinerea",      "Vintererle" },
    { "Motacilla flava",        "Gulerle" },
    { "Emberiza citrinella",    "Gulspurv" },
    { "Emberiza schoeniclus",   "Sivspurv" },
    { "Emberiza calandra",      "Kornspurv" },
    { "Accipiter nisus",        "Spurvehauk" },
    { "Accipiter gentilis",     "Hønsehauk" },
    { "Buteo buteo",            "Musvåk" },
    { "Falco tinnunculus",      "Tårnfalk" },
    { "Falco subbuteo",         "Lerkefalk" },
    { "Falco peregrinus",       "Vandrefalk" },
    { "Falco columbarius",      "Dvergfalk" },
    { "Strix nebulosa",         "Lappugle" },
    { "Dendrocopos major",      "Flaggspett" },
    { "Picus viridis",          "Grønnspett" },
    { "Cuculus canorus",        "Gjøk" },
    { "Apus apus",              "Tårnseiler" },
    { "Hirundo rustica",        "Låvesvale" },
    { "Delichon urbicum",       "Taksvale" },
    { "Regulus regulus",        "Fuglekonge" },
    { "Regulus ignicapilla",    "Ildkronge" },
    { "Phylloscopus collybita", "Gransanger" },
    { "Sylvia atricapilla",     "Munk" },
    { "Sylvia communis",        "Tornsanger" },
    { "Muscicapa striata",      "Gråfluesnapper" },
    { "Ficedula hypoleuca",     "Svarthvit fluesnapper" },
    { "Phoenicurus phoenicurus","Rødstjert" },
    { "Phoenicurus ochruros",   "Svartrødstjert" },
    { "Saxicola rubetra",       "Buskskvett" },
    { "Saxicola rubicola",      "Svarthaket buskskvett" },
    { "Oenanthe oenanthe",      "Steinskvett" },
    { "Lanius collurio",        "Tornskate" },
    { "Lanius excubitor",       "Varsler" },
    { "Bombycilla garrulus",    "Sidensvans" },
    { "Alauda arvensis",        "Sanglerke" },
    { "Vanellus vanellus",      "Vipe" },
    { "Phasianus colchicus",    "Fasan" },
    { "Ardea cinerea",          "Gråhegre" },
    { "Cygnus olor",            "Knoppsvane" },
    { "Cygnus cygnus",          "Sangsvane" },
    { "Anas platyrhynchos",     "Stokkand" },
    { "Anas crecca",            "Krikkand" },
    { "Anas penelope",          "Brunnakke" },
    { "Larus canus",            "Fiskemåke" },
    { "Larus argentatus",       "Gråmåke" },
    { "Larus marinus",          "Havmåke" },
    { "Larus fuscus",           "Sildemåke" },
    { "Sterna hirundo",         "Makrellterne" },

    /* Boreal/taiga specialists (added 2026-07-09, FSD v1.40 scoping) — the
     * mostly lowland garden/feeder set above misses species plausible in
     * forested/mountain terrain. Of these, Acanthis flammea, Loxia
     * curvirostra, Pinicola enucleator, Coccothraustes coccothraustes,
     * Surnia ulula and Aquila chrysaetos are already classes in the v1
     * 965-species model, so region-filtered IDs for them start working
     * immediately. The rest — including Perisoreus infaustus, the species
     * that prompted this audit — are NOT in the v1 model's class list
     * (confirmed against the actual label file) and stay inert here (never
     * matched at inference, since the model can't emit a class it was never
     * trained on) until a retrained model that includes them is installed;
     * see docs/MODEL.md and FSD §3.2.1. */
    { "Perisoreus infaustus",       "Lavskrike" },
    { "Poecile montanus",           "Granmeis" },
    { "Poecile cinctus",            "Lappmeis" },
    { "Lophophanes cristatus",      "Toppmeis" },
    { "Certhia familiaris",         "Trekryper" },
    { "Acanthis flammea",           "Gråsisik" },
    { "Turdus iliacus",             "Rødvingetrost" },
    { "Loxia curvirostra",          "Grankorsnebb" },
    { "Loxia pytyopsittacus",       "Furukorsnebb" },
    { "Pinicola enucleator",        "Konglebit" },
    { "Coccothraustes coccothraustes","Kjernebiter" },
    { "Dryocopus martius",          "Svartspett" },
    { "Picoides tridactylus",       "Tretåspett" },
    { "Dendrocopos minor",          "Dvergspett" },
    { "Nucifraga caryocatactes",    "Nøttekråke" },
    { "Bonasa bonasia",             "Jerpe" },
    { "Tetrao urogallus",           "Storfugl" },
    { "Lyrurus tetrix",             "Orrfugl" },
    { "Aegolius funereus",          "Perleugle" },
    { "Strix uralensis",            "Slagugle" },
    { "Glaucidium passerinum",      "Spurveugle" },
    { "Surnia ulula",               "Haukugle" },
    { "Aquila chrysaetos",          "Kongeørn" },
    { "Haliaeetus albicilla",       "Havørn" },

    /* Open-country / mountain / mire species from the user's target list
     * (Artsliste.txt, added 2026-07-17) not covered above — offered in the
     * relabel picker as hand-label ground-truth targets (FSD §3.2.1). Inert
     * at inference until a model that emits them is installed. */
    { "Anthus pratensis",           "Heipiplerke" },
    { "Pluvialis apricaria",        "Heilo" },
    { "Eremophila alpestris",       "Fjellerke" },
    { "Luscinia svecica",           "Blåstrupe" },
};
#define NO_NAMES_N (sizeof(NO_NAMES) / sizeof(NO_NAMES[0]))

bool species_in_region(const char *latin)
{
    if (!latin || !latin[0]) return false;
    for (size_t i = 0; i < NO_NAMES_N; i++)
        if (strcmp(NO_NAMES[i].latin, latin) == 0) return true;
    return false;
}

static const char *no_sentinel(const char *name_en)
{
    if (strcmp(name_en, "no bird") == 0)           return "ingen fugl";
    if (strcmp(name_en, "Unidentified bird") == 0) return "Uidentifisert fugl";
    if (strcmp(name_en, "unclassified") == 0)      return "ikke klassifisert";
    return NULL;
}

void species_localize(const char *name_en, const char *latin,
                       species_lang_t lang, char *out, size_t outsz)
{
    if (!name_en) name_en = "";
    if (!latin || !latin[0]) {
        const char *s = (lang == LANG_NO) ? no_sentinel(name_en) : NULL;
        strlcpy(out, s ? s : name_en, outsz);
        return;
    }
    const char *name = name_en;
    if (lang == LANG_NO)
        for (size_t i = 0; i < NO_NAMES_N; i++)
            if (strcmp(NO_NAMES[i].latin, latin) == 0) { name = NO_NAMES[i].no; break; }
    snprintf(out, outsz, "%s (%s)", name, latin);
}
