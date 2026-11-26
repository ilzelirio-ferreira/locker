# Locker ESP32

Controlador de fechadura para ESP32-2432S028 com display ILI9341, toque resistivo XPT2046 e leitor AS608. O acesso local usa um PIN de 6 a 12 dígitos ou uma digital cadastrada. A variante Matter expõe um dispositivo **Door Lock** pela rede Wi-Fi. A interface e a pinagem do display seguem a placa usada no Cofre Esp32 LCD. O projeto usa ESP-IDF, ESP-Matter e compilação no GitHub Actions, seguindo a estrutura do doorBell.

## Ligações

| Sinal | GPIO do CYD | Ligação |
|---|---:|---|
| RX UART2 | 27 | TX do AS608 |
| TX UART2 | 22 | RX do AS608 |
| GND | — | GND comum do AS608 e do driver |
| Saída da fechadura | 26 | Entrada de driver ativo em HIGH, em nível lógico de 3,3 V |

O AS608 usa UART 8N1 e endereço padrão. O firmware tenta 57.600, 9.600, 19.200, 38.400 e 115.200 baud e usa a primeira velocidade que retorna uma resposta válida. Confirme a alimentação, a ordem dos terminais e os níveis UART da sua versão do sensor antes de ligar. Use conversão de nível se a saída do leitor exceder 3,3 V. Alimente o leitor conforme sua especificação e evite identificar fios apenas pela cor.

O GPIO26 é compartilhado com o circuito do alto-falante em placas CYD comuns. Confirme acesso ao pino na sua revisão e deixe o alto-falante desconectado. Use um driver adequado à fechadura, com entrada mantida em LOW durante a partida, alimentação dimensionada para a carga e proteção contra retorno da bobina. Confira a lógica com a fechadura desconectada antes de energizar a carga. O firmware foi desenhado para uma fechadura que libera quando energizada e trava sem energia. O microSD fica fora deste projeto.

## Uso

1. Na primeira inicialização, toque nos quatro alvos e crie o PIN com a instalação sob seu controle. Confirme o mesmo PIN em seguida.
2. Digite o PIN e pressione **OK**, ou encoste uma digital cadastrada. A saída fica ativa por 5 segundos. Cinco entradas incorretas pelo teclado suspendem novas tentativas de PIN por 30 segundos durante a sessão atual.
3. Entre em **AJUSTES** com o mesmo PIN para alterá-lo, cadastrar até 10 digitais ou remover todas. Cada cadastro exige duas leituras do mesmo dedo. Os modelos ficam no AS608. O ESP32 guarda os identificadores habilitados e o hash do PIN com salt.
4. Para Matter, após criar o PIN, abra o monitor serial a 115200 baud e use o código de comissionamento informado pelo firmware com um controlador Matter que aceite dispositivos de desenvolvimento. Na variante Matter, o controlador autorizado pode liberar e travar a saída pelo endpoint Door Lock. O tempo local de 5 segundos também vale para a liberação remota. O estado exibido representa o comando da saída, pois o projeto não usa sensor de posição da porta.

## Compilação e gravação

O GitHub Actions publica os binários e argumentos de gravação como artefatos `locker-esp32-local` e `locker-esp32-matter`. Para compilar em um terminal com ESP-IDF exportado, use o alvo `esp32`. A variante Matter usa o ambiente Linux do ESP-Matter, com `ESP_MATTER_PATH` apontando para o SDK.

```sh
idf.py -B build-local -D SDKCONFIG=build-local/sdkconfig -D LOCKER_MATTER=OFF build
idf.py -B build-matter -D SDKCONFIG=build-matter/sdkconfig -D LOCKER_MATTER=ON build
```

Para gravar e acompanhar o boot, use `idf.py -B build-matter -p COM5 flash monitor`, ajustando o diretório da variante e a porta. Em Linux, substitua `COM5` pelo dispositivo serial correspondente. Use o bootloader, a tabela de partições e o aplicativo da mesma compilação. Uma limpeza completa da flash apaga o PIN, a lista de digitais habilitadas e o pareamento. Os modelos no AS608 persistem até serem removidos do leitor.
