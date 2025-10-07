using UnityEditor;
using UnityEngine;
using VRC.SDK3.Dynamics.Contact.Components;
using System.Linq;
using System.Collections.Generic;

// Defines the data structure for each haptic position mapping
[System.Serializable]
public class HapticBone
{
    public string Name;         // e.g., "Chest"
    public string Tag;          // e.g., "Chest"
    public string ParameterName;  // NEW: The VRChat OSC Parameter Name (e.g., "Receiver_Chest")
    public Transform Bone;      // The user-selected bone (Transform) for this position

    public HapticBone(string name, string tag)
    {
        Name = name;
        Tag = tag;
        // The parameter name is derived from the standard VRChat receiver name
        // This is CRUCIAL for OSC communication.
        ParameterName = $"Receiver_{name}"; 
        Bone = null;
    }
}

public class HapticReceiverPlacer : EditorWindow
{
    private const float CONTACT_RADIUS = 0.05f;
    private const string WINDOW_TITLE = "Haptic Receiver Placer";
    private const string MENU_PATH = "VRC_TOOLS/Haptic Receivers/Open Haptic Placer Window";

    private GameObject avatarRoot;
    
    // --- Receiver Position Definitions (Do Not Change) ---
    private static readonly (string name, string tag, HumanBodyBones unityBone)[] POSITION_DEFINITIONS = new (string, string, HumanBodyBones)[]
    {
        // Name (for parameter), Tag (for collision filter), Unity Bone
        
        // NEW: Added Head support
        ("Head", "Head", HumanBodyBones.Head),

        ("Chest", "Chest", HumanBodyBones.Chest),
        ("UpperArm_L", "UpperArm_L", HumanBodyBones.LeftUpperArm),
        ("UpperArm_R", "UpperArm_R", HumanBodyBones.RightUpperArm),
        ("Hips", "Hips", HumanBodyBones.Hips), 
        ("UpperLeg_L", "UpperLeg_L", HumanBodyBones.LeftUpperLeg),
        ("UpperLeg_R", "UpperLeg_R", HumanBodyBones.RightUpperLeg),
        ("LowerLeg_L", "LowerLeg_L", HumanBodyBones.LeftLowerLeg), 
        ("LowerLeg_R", "LowerLeg_R", HumanBodyBones.RightLowerLeg),
        ("Foot_L", "Foot_L", HumanBodyBones.LeftFoot), 
        ("Foot_R", "Foot_R", HumanBodyBones.RightFoot)
    };
    
    // --- Universal Sender Tags (Reduced List) ---
    private static readonly List<string> UNIVERSAL_SENDER_TAGS = new List<string>
    { 
        "Hand", 
        "Foot", 
    };

    // Runtime array to hold the user's bone selections
    private HapticBone[] hapticBones;
    
    // Used for scrolling the long list of bone fields
    private Vector2 scrollPosition;

    [MenuItem(MENU_PATH, false, 1)]
    private static void Init()
    {
        // Get existing open window or create a new one
        HapticReceiverPlacer window = (HapticReceiverPlacer)EditorWindow.GetWindow(typeof(HapticReceiverPlacer), false, WINDOW_TITLE);
        window.Show();
    }
    
    private void OnEnable()
    {
        InitializeBones();
        
        // Initial attempt to pre-populate the avatar root from selection
        if (Selection.activeGameObject != null && Selection.activeGameObject.GetComponentInParent<Animator>() != null)
        {
            avatarRoot = Selection.activeGameObject.transform.root.gameObject;
            if (avatarRoot != null)
            {
                AutoFindBones(false); // Do not show success dialog on initial load
            }
        }
    }
    
    // Initializes the hapticBones array based on the static definitions
    private void InitializeBones()
    {
        if (hapticBones == null || hapticBones.Length != POSITION_DEFINITIONS.Length)
        {
            hapticBones = new HapticBone[POSITION_DEFINITIONS.Length];
            for (int i = 0; i < POSITION_DEFINITIONS.Length; i++)
            {
                // Initialize HapticBone with Name, Tag, and derived ParameterName
                hapticBones[i] = new HapticBone(POSITION_DEFINITIONS[i].name, POSITION_DEFINITIONS[i].tag);
            }
        }
    }

    // --- Core Auto-Find Logic ---

    private void AutoFindBones(bool showDialog = true)
    {
        if (avatarRoot == null)
        {
            if (showDialog) EditorUtility.DisplayDialog("Error", "Please set the Avatar Root first to enable auto-finding.", "OK");
            return;
        }

        Animator animator = avatarRoot.GetComponentInChildren<Animator>();
        if (animator == null || !animator.isHuman)
        {
            if (showDialog) EditorUtility.DisplayDialog("Error", "Could not find a valid Humanoid Animator component on the Avatar Root.", "OK");
            return;
        }

        int bonesFound = 0;
        for (int i = 0; i < POSITION_DEFINITIONS.Length; i++)
        {
            // Use the Unity HumanBodyBones enumeration to find the bone Transform
            Transform bone = animator.GetBoneTransform(POSITION_DEFINITIONS[i].unityBone);
            if (bone != null)
            {
                hapticBones[i].Bone = bone;
                bonesFound++;
            }
        }

        Repaint();
        
        if (showDialog)
        {
            EditorUtility.DisplayDialog("Auto-Find Complete", 
                                         $"Found and mapped {bonesFound}/{POSITION_DEFINITIONS.Length} bones on {avatarRoot.name}.", 
                                         "OK");
        }
        else
        {
            Debug.Log($"Auto-Find completed for {avatarRoot.name}. Found and mapped {bonesFound}/{POSITION_DEFINITIONS.Length} bones.");
        }
    }

    // --- Batch Placement Logic ---
    
    /// <summary>
    /// Attempts to place receivers on all mapped bones.
    /// </summary>
    private void PlaceAllReceivers()
    {
        InitializeBones();
        if (hapticBones == null || hapticBones.Length == 0) return;

        // Filter for bones that are actually mapped and don't already have a receiver with the correct PARAMETER name
        var bonesToPlace = hapticBones.Where(b => b.Bone != null && 
                                                 !b.Bone.GetComponentsInChildren<VRCContactReceiver>()
                                                     .Any(r => r.parameter == b.ParameterName))
                                     .ToList();

        int totalToPlace = bonesToPlace.Count;
        if (totalToPlace == 0)
        {
            EditorUtility.DisplayDialog("Warning", "No new receivers needed. All currently mapped bones either have a receiver with the correct OSC parameter or are not mapped.", "OK");
            return;
        }

        int placedCount = 0;
        // Start a single, unified undo group for the batch operation
        Undo.IncrementCurrentGroup();
        int mainUndoGroup = Undo.GetCurrentGroup();

        EditorUtility.DisplayProgressBar("Placing Haptic Receivers", "Starting placement...", 0f);

        try
        {
            for (int i = 0; i < bonesToPlace.Count; i++)
            {
                HapticBone boneMapping = bonesToPlace[i];
                string progressTitle = $"Placing receiver on {boneMapping.Name}... ({i + 1}/{totalToPlace})";
                EditorUtility.DisplayProgressBar("Placing Haptic Receivers", progressTitle, (float)i / totalToPlace);
                
                // Call the internal placement logic, suppressing individual dialogs
                _PlaceReceiverInternal(boneMapping, false);
                placedCount++;
            }
        }
        finally
        {
            EditorUtility.ClearProgressBar();
        }

        Undo.SetCurrentGroupName("Place All Haptic Receivers");
        Undo.CollapseUndoOperations(mainUndoGroup);

        EditorUtility.DisplayDialog("Placement Complete", 
                                     $"Successfully placed {placedCount} new receivers.", 
                                     "Done!");
        Repaint(); // Refresh the GUI
    }

    // --- Editor Window GUI ---

    private void OnGUI()
    {
        InitializeBones();

        GUILayout.Label("Avatar Configuration", EditorStyles.boldLabel);
        
        GameObject oldAvatarRoot = avatarRoot;

        // Draw the Object field, potentially updating avatarRoot
        avatarRoot = (GameObject)EditorGUILayout.ObjectField(
            "Avatar Root (Required for Auto-Find):", 
            avatarRoot, 
            typeof(GameObject), 
            true
        );

        if (avatarRoot != oldAvatarRoot && avatarRoot != null)
        {
            // Clear current bone mappings
            foreach (var bone in hapticBones)
            {
                bone.Bone = null;
            }
            // Auto-find immediately on new avatar selection (no dialog shown)
            AutoFindBones(false);
        }

        // --- Auto-Find & Place All Buttons ---
        GUI.enabled = avatarRoot != null;
        if (GUILayout.Button("Manually Re-run Auto-Find Bones", GUILayout.Height(25)))
        {
            AutoFindBones(true);
        }
        
        bool anyBonesMapped = hapticBones != null && hapticBones.Any(b => b.Bone != null);
        GUI.enabled = avatarRoot != null && anyBonesMapped;

        EditorGUILayout.Space(5);
        if (GUILayout.Button("Place Receivers on ALL MAPPED Bones", GUILayout.Height(35)))
        {
            PlaceAllReceivers();
        }
        GUI.enabled = true; 

        EditorGUILayout.Space(15);
        
        if (avatarRoot == null)
        {
            EditorGUILayout.HelpBox("Select your Avatar Root above to automatically find and map the bones.", MessageType.Info);
        }

        GUILayout.Label("Bone Mapping Status:", EditorStyles.boldLabel);
        
        scrollPosition = EditorGUILayout.BeginScrollView(scrollPosition);

        bool allMapped = true;
        
        // Create an Object Field and a Button for each haptic position
        for (int i = 0; i < hapticBones.Length; i++)
        {
            HapticBone boneMapping = hapticBones[i];
            
            EditorGUILayout.BeginHorizontal();
            
            // 1. Bone Selection Field (shows the generated OSC Parameter name)
            boneMapping.Bone = (Transform)EditorGUILayout.ObjectField(
                $"{boneMapping.Name} ({boneMapping.ParameterName}):", 
                boneMapping.Bone, 
                typeof(Transform), 
                true
            );
            
            hapticBones[i] = boneMapping;

            // 2. Placement Button
            GUI.enabled = boneMapping.Bone != null;
            if (GUILayout.Button("Place Receiver", GUILayout.Width(120)))
            {
                // Individual placement calls the internal method AND shows a dialog.
                _PlaceReceiverInternal(boneMapping, true);
            }
            GUI.enabled = true;

            EditorGUILayout.EndHorizontal();
            
            if (boneMapping.Bone == null)
            {
                allMapped = false;
            }
        }
        
        EditorGUILayout.EndScrollView();

        EditorGUILayout.Space(15);
        
        // General instructions and status
        if (allMapped)
        {
            EditorGUILayout.HelpBox("All positions are mapped! Click the 'Place All' button above or 'Place Receiver' next to each one to add the contact component.", MessageType.Info);
        }
        else if (avatarRoot != null)
        {
            EditorGUILayout.HelpBox("Some bones are missing. Drag the corresponding bone from the Hierarchy into the empty slot, then click 'Place All' or individual 'Place Receiver' buttons.", MessageType.Warning);
        }
    }

    // --- Core Placement Logic ---

    /// <summary>
    /// Internal logic for creating and configuring the VRCContactReceiver.
    /// </summary>
    private void _PlaceReceiverInternal(HapticBone boneMapping, bool showDialog)
    {
        Transform boneTransform = boneMapping.Bone;
        string conciseName = boneMapping.Name;
        string collisionTag = boneMapping.Tag;
        string parameterName = boneMapping.ParameterName; // The new, essential parameter name
        
        if (boneTransform == null)
        {
            if (showDialog) EditorUtility.DisplayDialog("Error", $"The bone Transform is not set for '{conciseName}'.", "OK");
            return;
        }
        
        // Check if the receiver already exists with the correct parameter name
        if (boneTransform.GetComponentsInChildren<VRCContactReceiver>() 
                                 .Any(r => r.parameter == parameterName))
        {
            if (showDialog) 
            {
                EditorUtility.DisplayDialog("Warning", 
                                             $"A VRCContactReceiver with parameter '{parameterName}' already exists on or under {boneTransform.name}. Skipping.", 
                                             "OK");
            }
            return;
        }
        
        string contactObjectName = $"Receiver_{conciseName}"; 
        
        int undoGroup = 0;
        if (showDialog) 
        {
             Undo.IncrementCurrentGroup();
             Undo.SetCurrentGroupName($"Place Haptic Receiver: {conciseName}");
             undoGroup = Undo.GetCurrentGroup();
        }

        // 1. Create the new child GameObject
        GameObject receiverGO = new GameObject(contactObjectName);
        Undo.RegisterCreatedObjectUndo(receiverGO, $"Create Receiver GO {contactObjectName}");
        
        // Set it as a child of the target bone and zero out its position
        receiverGO.transform.SetParent(boneTransform);
        receiverGO.transform.localPosition = Vector3.zero;
        receiverGO.transform.localRotation = Quaternion.identity;
        receiverGO.transform.localScale = Vector3.one; 

        // 2. Add the VRCContactReceiver component and register the action for Undo
        VRCContactReceiver receiver = Undo.AddComponent<VRCContactReceiver>(receiverGO);
        
        // 3. Configure the component
        receiver.receiverType = VRCContactReceiver.ReceiverType.Proximity;
        receiver.radius = CONTACT_RADIUS;
        
        // ❗ CRITICAL FIX: Explicitly set the VRChat Parameter Name
        // This parameter name (e.g., "Receiver_Hips") is what VRChat sends over OSC!
        receiver.parameter = parameterName;
        
        // Set the collision tags
        receiver.collisionTags = UNIVERSAL_SENDER_TAGS;
        
        Debug.Log($"Successfully added VRCContactReceiver for '{conciseName}'. Parameter: '{parameterName}'.");
        
        if (showDialog) 
        {
             Undo.CollapseUndoOperations(undoGroup);
             EditorUtility.DisplayDialog("Success!", 
                                         $"Haptic Receiver for '{conciseName}' successfully added to {boneTransform.name}.\nComponent Name: {contactObjectName}.\n\nOSC Parameter: {parameterName} (Proximity)\n\nThis receiver now responds to {UNIVERSAL_SENDER_TAGS.Count} standard VRChat sender tags.", 
                                         "Done!");
        }
    }
}
