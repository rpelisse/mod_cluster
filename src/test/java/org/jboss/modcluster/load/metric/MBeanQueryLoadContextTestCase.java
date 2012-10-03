/*
 * JBoss, Home of Professional Open Source.
 * Copyright 2008, Red Hat Middleware LLC, and individual contributors
 * as indicated by the @author tags. See the copyright.txt file in the
 * distribution for a full listing of individual contributors.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA, or see the FSF site: http://www.fsf.org.
 */
package org.jboss.modcluster.load.metric;

import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;

import javax.management.MBeanServer;
import javax.management.MalformedObjectNameException;
import javax.management.ObjectName;

import org.easymock.EasyMock;
import org.jboss.modcluster.load.metric.impl.MBeanQueryLoadContext;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;

/**
 * @author Paul Ferraro
 *
 */
public class MBeanQueryLoadContextTestCase
{
   private MBeanServer server = EasyMock.createStrictMock(MBeanServer.class);
   private ObjectName name1;
   private ObjectName name2;
   private MBeanQueryLoadContext context1;
   private MBeanQueryLoadContext context2;
   
   @Before
   public void construct() throws MalformedObjectNameException
   {
      this.name1 = ObjectName.getInstance("domain:name=test1");
      this.name2 = ObjectName.getInstance("domain:name=test2");

      EasyMock.replay(this.server);
      
      this.context1 = new MBeanQueryLoadContext(this.server, this.name1);
      
      EasyMock.verify(this.server);
      EasyMock.reset(this.server);

      ObjectName pattern = ObjectName.getInstance("domain:*");
      
      EasyMock.expect(this.server.queryNames(pattern, null)).andReturn(new LinkedHashSet<ObjectName>(Arrays.asList(this.name1, this.name2)));
      
      EasyMock.replay(this.server);
      
      this.context2 = new MBeanQueryLoadContext(this.server, pattern);
      
      EasyMock.verify(this.server);
      EasyMock.reset(this.server);
   }
   
   @Test
   public void getAttributes() throws Exception
   {
      String expected = "value";
      
      EasyMock.expect(this.server.getAttribute(this.name1, "attribute")).andReturn(expected);

      EasyMock.replay(this.server);
      
      List<String> values = this.context1.getAttributes("attribute", String.class);
      
      EasyMock.verify(this.server);
      
      Assert.assertEquals(1, values.size());
      Assert.assertSame(expected, values.get(0));
      
      EasyMock.reset(this.server);
      

      String expected1 = "value1";
      String expected2 = "value2";
      
      EasyMock.expect(this.server.getAttribute(this.name1, "attribute")).andReturn(expected1);
      EasyMock.expect(this.server.getAttribute(this.name2, "attribute")).andReturn(expected2);

      EasyMock.replay(this.server);
      
      values = this.context2.getAttributes("attribute", String.class);
      
      EasyMock.verify(this.server);
      
      Assert.assertEquals(2, values.size());
      Assert.assertSame(expected1, values.get(0));
      Assert.assertSame(expected2, values.get(1));
      
      EasyMock.reset(this.server);
   }
   
   @Test
   public void close()
   {
      EasyMock.replay(this.server);
      
      this.context1.close();
      
      EasyMock.verify(this.server);
      EasyMock.reset(this.server);      

      
      EasyMock.replay(this.server);
      
      this.context2.close();
      
      EasyMock.verify(this.server);
      EasyMock.reset(this.server);      
   }
}