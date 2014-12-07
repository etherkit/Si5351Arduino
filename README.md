Si5351 Library for Arduino
==========================
This is a basic library for the Si5351 series of clock generator ICs from [Silicon Labs](1) for the Arduino development environment. It will allow you to control the Si5351 with an Arduino, and without depending on the proprietary ClockBuilder software from Silicon Labs.

This library is focused towards usage in RF/amateur radio applications, but it may be useful in other cases. However, keep in mind that coding decisions are and will be made with those applications in mind first, so if you need something a bit different, please do fork this repository. Also, since the Si5351A3 version is the one which seems most useful in amateur radio applications, this is where the current development will be focused. Once the Si5351A3 has a decent and mature feature set, hopefully we will be able to turn to the 8-output version, and perhaps even the B and C variants.

The library is currently in a minimalistic and bit of an untested state. Many of the features are not yet implemented. Unimplemented features will added according to usefulness in RF/amateur radio applications, as stated above. Please feel free to use the issues feature of GitHub if you run into problems or have suggestions for important features to implement.

Hardware Requirements and Setup
-------------------------------
This library has been written for the Arduino platform and has been successfully tested on the Arduino Uno and an Uno clone. There should be no reason that it would not work on any other Arduino hardware with I2C support.

The Si5351 is a +3.3 V only part, so if you are not using a +3.3 V microcontroller, be sure you have some kind of level conversion strategy.

Wire the SDA and SCL pins of the Si5351 to the corresponding pins on the Arduino. Use the pin assignments posted on the [Arduino Wire library page](http://arduino.cc/en/Reference/Wire). Since the I2C interface is set to 100 kHz, use 1 to 10 k&Omega; pullup resistors from +3.3 V to the SDA and SCL lines.

Connect a 25 MHz or 27 MHz crystal with a load capacitance of 6, 8, or 10 pF to the Si5351 XA and XB pins. Locate the crystal as close to the Si5351 as possible and keep the traces as short as possible. Please use a SMT crystal. A crystal with leads will have too much stray capacitance.

Example
-------
Include the Si5351Arduino library into your instance of the Arduino IDE. Download a ZIP file of the library from the GitHub repository by using the "Download ZIP" button at the right of the main repository page. Extract the ZIP file, then rename the unzipped folder as "Si5351". Finally, open the Arduino IDE, select menu Sketch > Import Library... > Add Library..., and select the renamed folder that you just downloaded. Restart the IDE and you should have access to the new library.

(Yes, the Arduino IDE can import a ZIP file, but it doesn't like filenames with characters such as dashes, as GitHub does when it appends the branch name with a dash. Perhaps there's an elegant way around this, we'll see.)

There is a simple example that is placed in your examples menu under Si5351Arduino. Open this to see how to initialize the Si5351 and set a couple of the outputs to different frequencies. The commentary below will analyze the sample sketch.

Before you do anything with the Si5351, you will need to include the "si5351.h" and "Wire.h" header files and instantiate the Si5351 class.

    #include "si5351.h"
    #include "Wire.h"

    Si5351 si5351;

Now in the Setup() function, let's initialize communications with the Si5351 and specify the load capacitance of the reference crystal:

    si5351.init(SI5351_CRYSTAL_LOAD_8PF);

Next, let's set the CLK0 output to 14 MHz and use a fixed PLL reference frequency (this allows for glitch-free tuning):

    si5351.set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
    si5351.set_freq(14000000, SI5351_PLL_FIXED, SI5351_CLK0);

The second value passed in the above method is the desired driving PLL frequency. Entering a 0 will have the method choose a PLL frequency for you. If you would like to use a fixed PLL frequency to drive a multisynth (in order to ensure glitch-free tuning), set the desired PLL frequency first using the method below, then specify that frequency in the set_freq() method. The PLL frequency only needs to be set once. Any additional frequency changes only need to use the set_freq() method as long as you are using the same PLL frequency as before.

Now let's set the CLK1 output to 20 MHz output, and let the Si5351 class pick a PLL frequency:

    si5351.set_freq(20000000, 0, SI5351_CLK1);

In the main Loop(), we use the Serial port to monitor the status of the Si5351, using a method to update a public struct which holds the status bits:

    si5351.update_status();
    Serial.print("SYS_INIT: ");
    Serial.print(si5351.dev_status.SYS_INIT);
    Serial.print("  LOL_A: ");
    Serial.print(si5351.dev_status.LOL_A);
    Serial.print("  LOL_B: ");
    Serial.print(si5351.dev_status.LOL_B);
    Serial.print("  LOS: ");
    Serial.print(si5351.dev_status.LOS);
    Serial.print("  REVID: ");
    Serial.println(si5351.dev_status.REVID);

Further Details
---------------
If we like we can adjust the output drive power:

    si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_4MA);

Individual outputs can be turned on and off. In the second argument, use a 0 to disable and 1 to enable:

    si5351.clock_enable(SI5351_CLK0, 0);

Also, there will be some inherent error in the reference crystal's actual frequency, so we can measure the difference between the actual and nominal output frequency in Hz, multiply by 10, make it an integer, and enter this correction factor into the library to store in EEPROM for future use. Once this is stored, it should not need to be set again unless you want to redo the calibration. With an accurate measurement at one frequency, this calibration should be good across the entire tuning range:

    si5351.set_correction(-900);

You can also use the get_correction() method to check the EEPROM to see if there is a non-zero correction factor stored, then skip setting the correction factor if there's already something other than the default (such as at startup, for example):

    int32_t corr_factor = si5351.get_correction()

One thing to note: the library is set for a 25 MHz reference crystal. If you are using a 27 MHz crystal, please change the SI5351_XTAL_FREQ define in si5351.h.

Tokens
------
Here are some of the defines, structs, and enumerations you will find handy to use with the library.

Crystal load capacitance:

    SI5351_CRYSTAL_LOAD_6PF
    SI5351_CRYSTAL_LOAD_8PF
    SI5351_CRYSTAL_LOAD_10PF

Clock outputs:

    enum si5351_clock {SI5351_CLK0, SI5351_CLK1, SI5351_CLK2, SI5351_CLK3,
      SI5351_CLK4, SI5351_CLK5, SI5351_CLK6, SI5351_CLK7};

PLL sources:

    enum si5351_pll {SI5351_PLLA, SI5351_PLLB};

Drive levels:

    enum si5351_drive {SI5351_DRIVE_2MA, SI5351_DRIVE_4MA, SI5351_DRIVE_6MA, SI5351_DRIVE_8MA};

Status register

    struct Si5351Status
    {
      uint8_t SYS_INIT;
      uint8_t LOL_B;
      uint8_t LOL_A;
      uint8_t LOS;
      uint8_t REVID;
    };

Interrupt register

    struct Si5351IntStatus
    {
      uint8_t SYS_INIT_STKY;
      uint8_t LOL_B_STKY;
      uint8_t LOL_A_STKY;
      uint8_t LOS_STKY;
    };

Oddities
--------
Right now, this code is focused solely on the 3-output 10-MSOP variant (Si5351A3). Since some of the code was derived from the Si5351 driver in the Linux kernel, it may be useable on with the other variants, but certainly many features won't work yet. With any luck, we will get the library to work with the other variants as well, or even better, maybe someone will take the initiative, write the code, and send me a pull request.

TODO
----
 - [ ] Implement tuning below 1 MHz
 - [ ] Implement tuning above 150 MHz
 - [ ] Implement phase register access
 - [ ] Create an interface to the ref osc frequency
 - [ ] Implement invert
 - [ ] Implement power up/down
 - [ ] Implement reset

  [1]: http://www.silabs.com
