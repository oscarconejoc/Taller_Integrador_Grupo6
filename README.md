## Máquina de estados del firmware

```mermaid
stateDiagram-v2
    Inicio --> ESPERA_GPS
    ESPERA_GPS --> LEER_DATA
    LEER_DATA --> EMPAQUETADO
    EMPAQUETADO --> TX_DATA
    TX_DATA --> SLEEP
    SLEEP --> ESPERA_GPS
    TX_DATA --> ERROR
    ERROR --> Inicio




## Diagrama de bloques del firmware
```mermaid
flowchart LR
    A[Main / Loop] --> B[Gestor de Estados]

    B --> C[Lectura GPS]
    C --> D[Procesamiento de Datos]
    D --> E[Empaquetado]
    E --> F[Transmisión LoRa / APRS]

    B --> G[Gestión de Energía]
    B --> H[Debug / Logging]

    G --> B
    F --> H
    D --> H
