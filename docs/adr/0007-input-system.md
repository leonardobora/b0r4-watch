# ADR 0007: Sistema de input — gestos + botão + voz, sem always-on

## Contexto
Precisávamos definir como o usuário interage com o relógio de forma que seja natural, econômica e não exija um app companion no celular.

## Decisão
Adotar um sistema de input híbrido:
- **Gestos físicos** via acelerômetro BMA423 (nativos e customizados).
- **Botão POWER** com ações por single, double e long press.
- **Touchscreen** para navegação em UI.
- **Voz** via WakeNet/MultiNet, mas **sob demanda/gesto**, não always-on.

O mapeamento gesto → ação é carregado de `gestures.json` no FATFS, permitindo customização por cliente/white-label.

## Consequências
- Bateria preservada: sem wake word sempre ouvindo e com wake por motion em hardware.
- Interação intencional: o usuário precisa fazer um gesto ou apertar um botão.
- White-label fácil: troca-se o profile de gestos sem recompilar.
- Exige implementação de um `GestureEngine` no firmware e detecção customizada de shake.
