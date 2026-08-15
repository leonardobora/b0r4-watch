# ADR 0004: Estratégia de IA — híbrido 3 camadas

## Contexto
Há interesse em inferência local e em IA assistida no relógio.

## Decisão
Adotar arquitetura híbrida de 3 camadas:
1. **On-device sempre ligado:** wake word (WakeNet9) e gestos (BMA423).
2. **On-device sob demanda:** comandos offline (MultiNet).
3. **Cloud via servidor:** ASR → LLM → TTS para linguagem livre.

LLM local no ESP32-S3 foi descartado como produto (experimentos atingem ~9,7 tok/s com modelos que não seguem instruções; é novidade, não ferramenta).

## Consequências
- Bateria preservada: wake word sob demanda e Wi-Fi só durante sessões de voz.
- Assistente natural language requer servidor próprio com chaves de API em `.env`.
- Wake word sempre ativo pode ser adicionado como opção futura.
