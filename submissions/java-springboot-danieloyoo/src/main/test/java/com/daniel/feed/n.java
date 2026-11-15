/*
package com.daniel.feed;

import org.apache.catalina.connector.Connector;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.boot.web.context.WebServerApplicationContext;
import org.springframework.boot.web.embedded.tomcat.TomcatWebServer;

import static org.junit.jupiter.api.Assertions.*;

// 🛠️ Changed to RANDOM_PORT to prevent port conflicts during automated builds
@SpringBootTest(webEnvironment = SpringBootTest.WebEnvironment.RANDOM_PORT)
class ConnectorConfigTest {

    @Autowired
    private WebServerApplicationContext context;

    @Test
    void testConnectorConfigurationsAreEqual() {
        assertTrue(context.getWebServer() instanceof TomcatWebServer, "Web server is not an instance of Tomcat");
        TomcatWebServer tomcatWebServer = (TomcatWebServer) context.getWebServer();

        Connector[] connectors = tomcatWebServer.getTomcat().getService().findConnectors();

        // Ensure we have both connectors initialized
        assertEquals(2, connectors.length, "Expected exactly 2 active connectors");

        Connector primary = connectors[0];
        Connector secondary = connectors[1];

        // Assert that the secondary connector completely mirrors the performance profile of the primary
        assertEquals(primary.getProperty("maxConnections"), secondary.getProperty("maxConnections"),
                "Max connections configuration mismatch!");

        assertEquals(primary.getProperty("acceptCount"), secondary.getProperty("acceptCount"),
                "Accept count configuration mismatch!");

        assertEquals(primary.getProperty("keepAliveTimeout"), secondary.getProperty("keepAliveTimeout"),
                "Keep alive timeout configuration mismatch!");
    }
}

*/