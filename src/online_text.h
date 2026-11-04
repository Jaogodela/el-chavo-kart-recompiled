#pragma once

#include <string_view>

#include "language_setting.h"

namespace eck {

// The online strings in the four language packages the game ships (INT, ESM, ESN, PTB), for the game's own menus.
// The launcher keeps its single pt-BR table (pt_br_ui_text.cpp) and stays English in the other languages.
enum class TextPackage { kInt, kEsm, kEsn, kPtb };

struct OnlineString {
  const char* key;  // the English text, also the INT text
  const char* esm;
  const char* esn;
  const char* ptb;
};

inline constexpr OnlineString kOnlineStrings[] = {
    {"Online", "En l\xC3\xADnea", "En l\xC3\xADnea", "Online"},
    {"Online play", "Juego en l\xC3\xADnea", "Juego en l\xC3\xADnea", "Jogo online"},
    {"Online name", "Nombre en l\xC3\xADnea", "Nombre en l\xC3\xADnea", "Nome online"},
    {"Your friend code", "Tu c\xC3\xB3" "digo de amigo", "Tu c\xC3\xB3" "digo de amigo", "Seu c\xC3\xB3" "digo de amigo"},
    {"Copy", "Copiar", "Copiar", "Copiar"},
    {"Connection test", "Prueba de conexi\xC3\xB3n", "Prueba de conexi\xC3\xB3n", "Teste de conex\xC3\xA3o"},
    {"Friends", "Amigos", "Amigos", "Amigos"},
    {"Friend requests", "Solicitudes de amistad", "Solicitudes de amistad", "Pedidos de amizade"},
    {"Send request", "Enviar solicitud", "Enviar solicitud", "Enviar pedido"},
    {"Accept", "Aceptar", "Aceptar", "Aceitar"},
    {"Decline", "Rechazar", "Rechazar", "Recusar"},
    {"Remove", "Eliminar", "Eliminar", "Remover"},
    {"Create Room", "Crear sala", "Crear sala", "Criar sala"},
    {"Find Room", "Buscar sala", "Buscar sala", "Procurar sala"},
    {"Invite Friends", "Invitar amigos", "Invitar amigos", "Convidar amigos"},
    {"Start Race", "Iniciar carrera", "Iniciar carrera", "Iniciar corrida"},
    {"Leave Room", "Salir de la sala", "Salir de la sala", "Sair da sala"},
    {"Waiting for players", "Esperando jugadores", "Esperando jugadores", "Aguardando jogadores"},
    {"Room full", "Sala llena", "Sala llena", "Sala cheia"},
    {"Player left", "Un jugador se fue", "Un jugador se ha marchado", "Jogador saiu"},
    {"Connection lost", "Conexi\xC3\xB3n perdida", "Conexi\xC3\xB3n perdida", "Conex\xC3\xA3o perdida"},
    {"Unable to connect", "No se pudo conectar", "No se pudo conectar", "N\xC3\xA3o foi poss\xC3\xADvel conectar"},
    {"Version mismatch", "Versiones incompatibles", "Versiones incompatibles", "Vers\xC3\xB5" "es incompat\xC3\xADveis"},
    {"Direct", "Directa", "Directa", "Direta"},
    {"Relay", "Retransmisi\xC3\xB3n", "Retransmisi\xC3\xB3n", "Retransmiss\xC3\xA3o"},
};

constexpr TextPackage PackageForLanguage(std::string_view language_key) {
  if (language_key == "pt-BR") return TextPackage::kPtb;
  if (language_key == "es-MX") return TextPackage::kEsm;
  if (language_key == "es-ES") return TextPackage::kEsn;
  return TextPackage::kInt;
}

// The text of `english` in `package`; an unknown key comes back unchanged.
constexpr const char* OnlineText(const char* english, TextPackage package) {
  for (const OnlineString& row : kOnlineStrings) {
    if (std::string_view(row.key) != english) continue;
    switch (package) {
      case TextPackage::kEsm: return row.esm;
      case TextPackage::kEsn: return row.esn;
      case TextPackage::kPtb: return row.ptb;
      default: return row.key;
    }
  }
  return english;
}

}  // namespace eck
