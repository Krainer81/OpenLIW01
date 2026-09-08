# Hardware interface summary

This file intentionally documents only the functional interface required by the independent firmware. It is not a PCB reproduction.

| GPIO | Function |
|---|---|
| GPIO0 | HC165 /CE |
| GPIO1 | HC165 /PL |
| GPIO2 | HC590 /CCLR physical branch; not used as a normal counter-reset action |
| GPIO3 | C755 /CLR |
| GPIO4 | C755 Q / overflow pending |
| GPIO5 | Local service button, active LOW |
| GPIO12 | HSPI MISO |
| GPIO13 | HSPI MOSI |
| GPIO14 | HSPI CLK |
| GPIO15 | FRAM /CS |
| GPIO16 | Green status LED, active LOW |

GPIO5 production mode: active LOW, internal pull-up enabled, interrupt mode enabled, 50 ms debounce. Service classification is release-only.

The project does not publish PCB photographs, board scans, Gerbers or vendor production artwork.
