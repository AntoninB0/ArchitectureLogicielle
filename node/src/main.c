#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(button0), gpios);

int main(void){

    bool error = false;
    if (!device_is_ready(led.port)) {
        printk("Error: LED device %s is not ready\n", led.port->name);
        error = true;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    
    if (!device_is_ready(button.port)) {
        printk("Error: Button device %s is not ready\n", button.port->name);
        error = true;
    }
    gpio_pin_configure_dt(&button, GPIO_INPUT);

    while (!error) {
        
        if (gpio_pin_get_dt(&button)) {
            gpio_pin_toggle_dt(&led);
        }
        k_msleep(100);
    }

}





