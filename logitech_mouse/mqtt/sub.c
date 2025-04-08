#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "MQTTClient.h"
#include <mysql/mysql.h>

#define ADDRESS     "tcp://broker.emqx.io:1883"
#define CLIENTID    "subcriber_mouse_driver"
#define SUB_TOPIC   "mouse_driver/speed_and_accuracy"

// #define QOS         1

MYSQL *conn;
MYSQL_RES *res;
MYSQL_ROW row;

char *server = "localhost";
char *user = "root";
char *password = "123456"; /* set me first */
char *database = "mouse_data";




int on_message(void *context, char *topicName, int topicLen, MQTTClient_message *message) {
    char* payload = message->payload;
    printf("Received message: %s\n", payload);
    
    conn = mysql_init(NULL);
    if (mysql_real_connect(conn, server, user, password, database, 0, NULL, 0) == NULL) 
    {
        fprintf(stderr, "%s\n", mysql_error(conn));
        mysql_close(conn);
        exit(1);
    }  
    float speed, accuracy;
    if (sscanf(payload, "{\"speed\": %f, \"accuracy\": %f}", &speed, &accuracy) == 2) {
        // printf("CPU Temperature: %.1f°C\n", cpu_temp);
        // printf("SSD Temperature: %.1f°C\n", ssd_temp);
        char sql[200];
        sprintf(sql,"insert into mouse_metrics(speed, accuracy) values (%.2f, %.2f)",speed, accuracy);
        mysql_query(conn,sql);
    }
    else
    {
        printf("Failed to parse message!\n");
    }
    

    
    mysql_close(conn);
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

int main(int argc, char* argv[]) {
    MQTTClient client;
    MQTTClient_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    //conn_opts.username = "your_username>>";
    //conn_opts.password = "password";

    MQTTClient_setCallbacks(client, NULL, NULL, on_message, NULL);

    int rc;
    if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS) {
        printf("Failed to connect, return code %d\n", rc);
        exit(-1);
    }
   
    //listen for operation
    MQTTClient_subscribe(client, SUB_TOPIC, 0);


    while(1) {
        //send temperature measurement
        // publish(client, PUB_TOPIC, "HELLO WORLD!");
        // sleep(3);
    }
    MQTTClient_disconnect(client, 1000);
    MQTTClient_destroy(&client);
    return rc;
}