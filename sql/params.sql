.separator "\t"

CREATE TABLE bands(
    id          INTEGER PRIMARY KEY,
    name        TEXT,
    start_freq  INTEGER,
    stop_freq   INTEGER,
    type        INTEGER
);

.import bands_ham.csv bands
.import bands_cb.csv bands
.import bands_broadcast.csv bands
.import bands_transverter.csv bands

CREATE TABLE band_params(
    bands_id    INTEGER,
    name        TEXT,
    val         INTEGER,
    UNIQUE      (bands_id, name) ON CONFLICT REPLACE
);

.import band_params.csv band_params

CREATE TABLE params(
    name        TEXT PRIMARY KEY ON CONFLICT REPLACE,
    val         INTEGER
);

.import params.csv params

CREATE TABLE mode_params(
    mode        INTEGER,
    name        TEXT,
    val         INTEGER,
    UNIQUE      (mode, name) ON CONFLICT REPLACE
);

.import mode_params.csv mode_params

CREATE TABLE atu(
    ant         INTEGER,
    freq        INTEGER,
    val         INTEGER,
    UNIQUE      (ant, freq) ON CONFLICT REPLACE
);

CREATE TABLE memory(
    id          INTEGER,
    name        TEXT,
    val         INTEGER,
    UNIQUE      (id, name) ON CONFLICT REPLACE
);

.import memory.csv memory

CREATE TABLE msg_cw(
    id          INTEGER PRIMARY KEY,
    val         INTEGER
);

.import msg_cw.csv msg_cw

CREATE TABLE transverter(
    id          INTEGER,
    name        TEXT,
    val         INTEGER,
    UNIQUE      (id, name) ON CONFLICT REPLACE
);
