// Tema (sempre escuro, nas cores da logo) e o ícone do programa.
#pragma once

#include <QIcon>

#include "common/config.h"

namespace framux::gui {

// Aplica o tema escuro à aplicação inteira.
void apply_theme();

// Ícone do programa (framux-icons/, embutido nos recursos :/framux).
// `state`: 0 ocioso, 1 benchmark (selo verde no canto), 2 gravando (selo
// vermelho, como o ícone do FRAPS durante a gravação).
QIcon app_icon(int state = 0);

// Nome "Framux" com as letras na cor `text` (o ponto continua vermelho).
QPixmap wordmark(int height, const QColor &text, qreal dpr);

}  // namespace framux::gui
