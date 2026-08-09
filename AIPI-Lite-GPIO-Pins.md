|     | Function                       | GPIO Pin | Notes                                                             |
| --- | ------------------------------ | -------- | ----------------------------------------------------------------- |
|     | **POWER MANAGEMENT**           |          |                                                                   |
|     | Power Control (Keep-Alive)     | GPIO10   | **CRITICAL:** Must be set HIGH on boot to stay powered on battery |
|     | Battery Voltage Monitor        | GPIO2    | ADC input, use 12db attenuation, multiply by 2.0                  |
|     |                                |          |                                                                   |
|     | **DISPLAY (ST7735 - SPI)**     |          |                                                                   |
|     | SPI Clock (SCK)                | GPIO16   |                                                                   |
|     | SPI Data (MOSI)                | GPIO17   |                                                                   |
|     | Chip Select (CS)               | GPIO15   |                                                                   |
|     | Data/Command (DC)              | GPIO7    |                                                                   |
|     | Reset                          | GPIO18   |                                                                   |
|     | Backlight PWM                  | GPIO3    | ⚠️ Strapping pin - works but shows warning                        |
|     |                                |          |                                                                   |
|     | **AUDIO CODEC (ES8311 - I2C)** |          |                                                                   |
|     | I2C Data (SDA)                 | GPIO5    |                                                                   |
|     | I2C Clock (SCL)                | GPIO4    |                                                                   |
|     | I2C Address                    | 0x18     | ES8311 codec address                                              |
|     |                                |          |                                                                   |
|     | **AUDIO (I2S)**                |          |                                                                   |
|     | I2S Master Clock (MCLK)        | GPIO6    |                                                                   |
|     | I2S Bit Clock (BCLK)           | GPIO14   |                                                                   |
|     | I2S Word Select (LRCLK)        | GPIO12   |                                                                   |
|     | I2S Data Out (DOUT)            | GPIO11   | To speaker                                                        |
|     | Speaker Amp Enable             | GPIO9    | Turn on before playing audio                                      |
|     |                                |          |                                                                   |
|     | **USER INPUTS**                |          |                                                                   |
|     | Left Button                    | GPIO1    | Also hardware power button (dual function)                        |
|     | Right Button                   | GPIO42   | Standard GPIO button                                              |