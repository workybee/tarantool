box.schema.role.create('iddqd')
box.schema.role.create('iddqd')
box.schema.role.drop('iddqd')
box.schema.role.drop('iddqd')
box.schema.role.create('iddqd')
-- impossible to su to a role
box.session.su('iddqd')
-- test granting privilege to a role
box.schema.role.grant('iddqd', 'execute', 'universe')
box.schema.role.info('iddqd')
box.schema.role.revoke('iddqd', 'execute', 'universe')
box.schema.role.info('iddqd')
-- test granting a role to a user
box.schema.user.create('tester')
box.schema.user.info('tester')
box.schema.user.grant('tester', 'execute', 'role', 'iddqd')
box.schema.user.info('tester')
-- test granting user to a user
box.schema.user.grant('tester', 'execute', 'role', 'tester')
-- test granting a non-execute grant on a role - error
box.schema.user.grant('tester', 'write', 'role', 'iddqd')
box.schema.user.grant('tester', 'read', 'role', 'iddqd')
-- test granting role to a role
box.schema.user.grant('iddqd', 'execute', 'role', 'iddqd')
box.schema.user.grant('iddqd', 'iddqd')
box.schema.user.revoke('iddqd', 'iddqd')
box.schema.user.grant('tester', 'iddqd')
box.schema.user.revoke('tester', 'iddqd')
box.schema.role.drop('iddqd')
box.schema.user.revoke('tester', 'no-such-role')
box.schema.user.grant('tester', 'no-such-role')
box.schema.user.drop('tester')
-- check for loops in role grants
box.schema.role.create('a')
box.schema.role.create('b')
box.schema.role.create('c')
box.schema.role.create('d')
box.schema.user.grant('b', 'a')
box.schema.user.grant('c', 'a')
box.schema.user.grant('d', 'b')
box.schema.user.grant('d', 'c')
box.schema.user.grant('a', 'd')
box.schema.role.drop('d')
box.schema.role.drop('b')
box.schema.role.drop('c')
box.schema.role.drop('a')
-- check that when dropping a role, it's first revoked
-- from whoever it is granted
box.schema.role.create('a')
box.schema.role.create('b')
box.schema.user.grant('b', 'a')
box.schema.role.drop('a')
box.schema.user.info('b')
box.schema.role.drop('b')
-- check a grant received via a role
box.schema.user.create('test')
box.schema.user.create('grantee')
box.schema.role.create('liaison')
box.schema.user.grant('grantee', 'liaison')
box.schema.user.grant('test', 'read,write', 'universe')
box.session.su('test')
s = box.schema.space.create('test')
s:create_index('i1')
box.schema.role.grant('liaison', 'read,write', 'space', 'test')
box.session.su('grantee')
box.space.test:insert{1}
box.space.test:select{1}
box.session.su('test')
box.schema.user.revoke('liaison', 'read,write', 'space', 'test')
box.session.su('grantee')
box.space.test:insert{1}
box.space.test:select{1}
box.session.su('admin')
box.schema.user.drop('test')
box.schema.user.drop('grantee')
box.schema.user.drop('liaison')


--
-- Test how privileges are propagated through a complex role graph.
-- Here's the graph:
--
-- role1 ->- role2 -->- role4 -->- role6 ->- user1
--                \               /     \
--                 \->- role5 ->-/       \->- role9 ->- role10 ->- user
--                     /     \               /
--           role3 ->-/       \->- role7 ->-/
--
-- Privilege checks verify that grants/revokes are propagated correctly
-- from the role1 to role10.
--
box.schema.user.create("user")
box.schema.role.create("role1")
box.schema.role.create("role2")
box.schema.role.create("role3")
box.schema.role.create("role4")
box.schema.role.create("role5")
box.schema.role.create("role6")
box.schema.role.create("role7")
box.schema.user.create("user1")
box.schema.role.create("role9")
box.schema.role.create("role10")

box.schema.role.grant("role2", "role1")
box.schema.role.grant("role4", "role2")
box.schema.role.grant("role5", "role2")
box.schema.role.grant("role5", "role3")
box.schema.role.grant("role6", "role4")
box.schema.role.grant("role6", "role5")
box.schema.role.grant("role7", "role5")
box.schema.role.grant("user1", "role6")
box.schema.role.grant("role9", "role6")
box.schema.role.grant("role9", "role7")
box.schema.role.grant("role10", "role9")
box.schema.user.grant("user", "role10")

-- try to create a cycle
box.schema.role.grant("role2", "role10")

--
-- test grant propagation
-- 
box.schema.role.grant("role1", "read", "universe")
box.session.su("user")
box.space._space.index.name:get{"_space"}[3]
box.session.su("admin")
box.schema.role.revoke("role1", "read", "universe")
box.session.su("user")
box.space._space.index.name:get{"_space"}[3]
box.session.su("admin")

--
-- space-level privileges
--
box.schema.role.grant("role1", "read", "space", "_index")
box.session.su("user")
box.space._space.index.name:get{"_space"}[3]
box.space._index:get{288, 0}[3]
box.session.su("admin")
box.schema.role.revoke("role1", "read", "space", "_index")
box.session.su("user")
box.space._space.index.name:get{"_space"}[3]
box.space._index:get{288, 0}[3]
box.session.su("admin")

--
-- grant to a non-leaf branch
--
box.schema.role.grant("role5", "read", "space", "_index")
box.session.su("user")
box.space._space.index.name:get{"_space"}[3]
box.space._index:get{288, 0}[3]
box.session.su("admin")
box.schema.role.revoke("role5", "read", "space", "_index")
box.session.su("user")
box.space._space.index.name:get{"_space"}[3]
box.space._index:get{288, 0}[3]
box.session.su("admin")

-- grant via two branches
--
box.schema.role.grant("role3", "read", "space", "_index")
box.schema.role.grant("role4", "read", "space", "_index")
box.schema.role.grant("role9", "read", "space", "_index")

box.session.su("user")
box.space._index:get{288, 0}[3]
box.session.su("user1")
box.space._index:get{288, 0}[3]

box.session.su("admin")
box.schema.role.revoke("role3", "read", "space", "_index")

box.session.su("user")
box.space._index:get{288, 0}[3]
box.session.su("user1")
box.space._index:get{288, 0}[3]

box.session.su("admin")
box.schema.role.revoke("role4", "read", "space", "_index")

box.session.su("user")
box.space._index:get{288, 0}[3]
box.session.su("user1")
box.space._index:get{288, 0}[3]

box.session.su("admin")
box.schema.role.revoke("role9", "read", "space", "_index")

box.session.su("user")
box.space._index:get{288, 0}[3]
box.session.su("user1")
box.space._index:get{288, 0}[3]
box.session.su("admin")

box.schema.user.drop("user")
box.schema.user.drop("user1")
box.schema.role.drop("role1")
box.schema.role.drop("role2")
box.schema.role.drop("role3")
box.schema.role.drop("role4")
box.schema.role.drop("role5")
box.schema.role.drop("role6")
box.schema.role.drop("role7")
box.schema.role.drop("role9")
box.schema.role.drop("role10")
