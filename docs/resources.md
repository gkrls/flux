## Switch Pipeline Resources (Edgecore DCS810, 20-stage Tofino2)

> **NOTE** The software that produced the info below, including its artifacts, is still consindered confidential by Intel, and thus NDAs still apply. Therefore, we can only share what we know is safe to share. Should Intel relax these restrictions we will update this page accordingly. 

![](img/resources/platform_compiler.png)

### Straggle-Aware

![](img/resources/sa_action_data_bus.png)

![](img/resources/sa_sram.png)

![](img/resources/sa_tcam.png)

![](img/resources/sa_vliw_and_others.png)

| Cycles | Parser info |
|:------:|:-----------:|
| ![Cycles](img/resources/sa_cycles.png) | ![Parsers](img/resources/sa_parsers.png) |

### Straggle-Oblivious (Standard INA like SwitchML etc.)


![](img/resources/so_action_data_bus.png)

![](img/resources/so_sram.png)

![](img/resources/so_tcam.png)

![](img/resources/so_vliw_and_others.png)


| Cycles | Parser info |
|:------:|:-----------:|
| ![Cycles](img/resources/so_cycles.png) | ![Parsers](img/resources/so_parsers.png) |