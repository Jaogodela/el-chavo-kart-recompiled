// The fixed ids race telemetry accepts. Ordering is the game script's: EKartCharacterType, EKartTracks and normalTracks in
// ChavoKartGame.xxx (docs/online-analysis.md, section 12). Character names are the game's pt-BR localization, track names its English one.
export const COMPETITIONS = [
  { id: 'normal_race', name: 'Normal Race' },
  { id: 'time_attack', name: 'Time Attack' },
  { id: 'survival', name: 'Survival' },
  { id: 'endurance', name: 'Endurance' },
  { id: 'coins', name: 'Coins' },
  { id: 'follow_the_leader', name: 'Follow the Leader' },
  { id: 'ctf', name: 'Capture the Flag' },
  { id: 'challenges', name: 'Challenges' },
  { id: 'challenges_ctf', name: 'Challenges CTF' },
];

export const CHARACTERS = [
  { id: 0, key: 'chavo', name: 'Chaves' },
  { id: 1, key: 'quico', name: 'Quico' },
  { id: 2, key: 'don_ramon', name: 'Seu Madruga' },
  { id: 3, key: 'dona_florinda', name: 'Dona Florinda' },
  { id: 4, key: 'profesor_jirafales', name: 'Professor Girafales' },
  { id: 5, key: 'popis', name: 'Pópis' },
  { id: 6, key: 'nono', name: 'Nhonho' },
  { id: 7, key: 'senor_barriga', name: 'Senhor Barriga' },
  { id: 8, key: 'dona_cleotilde', name: 'Dona Clotilde' },
  { id: 9, key: 'paty', name: 'Paty' },
  { id: 10, key: 'rufino', name: 'Rufino Malfeitor' },
  { id: 11, key: 'godinez', name: 'Godines' },
];

export const TRACKS = [
  { id: 0, map: 'PistaBrasilM_Circuito0', name: 'Brazil Maracana' },
  { id: 1, map: 'PistaCastillo_Circuito0', name: 'Castle of 71' },
  { id: 2, map: 'PistaTanga_Circuito1', name: 'Pueblito' },
  { id: 3, map: 'PistaMuseo_Circuito0', name: 'The Museum' },
  { id: 4, map: 'PistaCiudad_Circuito0', name: 'Chavoltepec' },
  { id: 5, map: 'PistaBrasilC_Circuito0', name: 'Brazilian Sky' },
  { id: 6, map: 'PistaAcapulco_Circuito0', name: 'Acapulco' },
  { id: 7, map: 'PistaCastillo_Circuito1', name: 'Haunted House' },
  { id: 8, map: 'PistaElEspacio_Circuito0', name: 'Space Route' },
  { id: 9, map: 'PistaLaFeria_Circuito0', name: 'The Funfair' },
  { id: 10, map: 'PistaHeroes_Circuito0', name: 'Heroic Wrestle' },
  { id: 11, map: 'PistaTanga_Circuito0', name: 'Tangamandapio' },
  { id: 12, map: 'PistaCiudad_Circuito1', name: 'The City' },
  { id: 13, map: 'PistaBrasilM_Circuito1', name: 'Soccer Fever' },
  { id: 14, map: 'PistaMuseo_Circuito1', name: 'The Gallery' },
  { id: 15, map: 'PistaBrasilC_Circuito1', name: 'Over Rio' },
  { id: 16, map: 'PistaAcapulco_Circuito1', name: 'The Coast' },
  { id: 17, map: 'PistaLaFeria_Circuito1', name: 'Theme Park' },
  { id: 18, map: 'PistaHeroes_Circuito1', name: "Hero's Gym" },
  { id: 19, map: 'PistaElEspacio_Circuito1', name: 'Milky Way' },
];

export const VALID_COMPETITIONS = new Set(COMPETITIONS.map((c) => c.id));
export const VALID_CHARACTERS = new Set(CHARACTERS.map((c) => c.id));
export const VALID_TRACKS = new Set(TRACKS.map((t) => t.id));

// What the dashboard script needs to label ids.
export const GAME_LABELS = {
  competitions: Object.fromEntries(COMPETITIONS.map((c) => [c.id, c.name])),
  characters: CHARACTERS.map((c) => c.name),
  tracks: TRACKS.map((t) => t.name),
};
