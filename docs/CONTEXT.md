# Contexto: b0r4-watch

Glossário de linguagem ubíqua para o projeto.

## Termos

### Watchface
Tela principal do relógio, mostra hora, data, bateria e notificações. Pode ter múltiplos temas selecionáveis.

### Launcher
Menu/app drawer acessível por gesto de touch. Contém os apps: música, luzes, IR, Wi-Fi, etc.

### App (no relógio)
Funcionalidade isolada no firmware, acessível pelo launcher. Ex.: app de música, app de luzes, app de IR.

### Gatilho
Ação que inicia uma sessão de IA/voz. Pode ser um gesto, pressionar o botão Power, ou um wake word.

### Intent
Comando de alto nível reconhecido localmente (MultiNet) ou inferido pelo servidor (LLM). Ex.: "ligar luz da sala".

### Stream de voz
Fluxo de áudio PDM do microfone para o servidor via WebSocket, para ASR/LLM/TTS.

### Notificação ANCS
Notificação recebida do iPhone via protocolo Apple Notification Center Service (BLE).

### Dual-boot (firmware vs Meshtastic)
Capacidade de flashear o relógio com o firmware próprio ou com o firmware Meshtastic, mas não ambos ao mesmo tempo.

### Modo de energia
Política de sleep/wake que equilibra funcionalidade e autonomia. Padrão: BLE ativo, Wi-Fi sob demanda, wake word sob demanda.

### Comunicador
Uso do rádio LoRa para comunicação ponto-a-ponto ou mesh (Meshtastic).

### Tema
Combinação de watchface, cores, fontes e layout da UI. Produzido no SquareLine Studio e exportado para LVGL.

### Gesture Engine
Módulo do firmware que escuta eventos de input (gestos, botão, touch, voz) e os traduz em Actions.

### Trigger
Padrão detectável que dispara uma ação. Ex.: `shake_x:2`, `double_click`, `wrist_tilt`.

### Action
Comando executado quando um trigger é reconhecido. Pode ser local, um intent ou uma mensagem para o servidor.

### Gesture Profile
Arquivo de configuração (`gestures.json`) que mapeia triggers em actions. Permite white-label por cliente.

### Input Source
Origem do evento de input: `ACCEL`, `BUTTON`, `TOUCH` ou `VOICE`.

### Custom Shake
Gesto detectado por software a partir dos dados brutos do acelerômetro, como "chacoalhar 2x pro lado".
